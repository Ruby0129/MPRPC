#include "rpcprovider.h"
#include "mprpcapplication.h"
#include "rpcheader.pb.h"
#include "logger.h"
#include "zookeeperutil.h"

/**
 * service_name  => service 描述
 *                         =》 service* 记录服务对象
 *                         =》 method_name => method对象方法
*/
void RpcProvider::NotifyService(google::protobuf::Service* service)
{
    ServiceInfo service_info;
    // 获取了服务对象的描述信息
    const google::protobuf::ServiceDescriptor* pserviceDesc = service->GetDescriptor();
    // 获取服务的名字
    std::string service_name = pserviceDesc->name();
    // 后去服务对象service的方法的数量
    int methodCnt = pserviceDesc->method_count();

    // std::cout << "service_name: " << service_name << std::endl;
    LOG_INFO("service_name:%s", service_name.c_str()); 

    for (int i=0; i < methodCnt; ++i)
    {
        // 获取了服务对象指定下标的服务方法的描述 （抽象描述)
        const google::protobuf::MethodDescriptor* pmethodDesc = pserviceDesc->method(i);
        std::string method_name = pmethodDesc->name();
        service_info.m_methodMap.insert({method_name, pmethodDesc});
        // std::cout << "method_name: " << method_name << std::endl;
        LOG_INFO("method_name:%s", method_name.c_str()); 
    }

    service_info.m_service = service;
    m_serviceMap.insert({service_name, service_info});
}

// 启动rpc服务节点，开始提供rpc远程网络调用服务
void RpcProvider::Run()
{
    // 读取配置文件rpcserver的信息
    std::string ip = MprpcApplication::GetInstance().GetConfig().Load("rpcserverip");
    uint16_t port = atoi(MprpcApplication::GetInstance().GetConfig().Load("rpcserverport").c_str());

    InetAddress address(port, ip);

    // 创建TcpServer对象
    TcpServer server(&m_eventLoop, address, "RpcProvider");
    // 绑定连接回调和消息读写回调方法 分离网络代码和业务代码
    server.setConnectionCallback(std::bind(&RpcProvider::OnConnection, this, std::placeholders::_1));
    server.setMessageCallback(std::bind(&RpcProvider::OnMessage, this, 
                            std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

    // 设置mymuduo库的线程数量
    server.setThreadNum(4);
    // std::cout << "PRCProviedr start service at ip:" << ip << " port:" << port << std::endl;
    
    // 把当前rpc节点上要发布的服务全部注册到zk上面，让rpc client可以从zk上发现服务
    // session timeout 30s zkclient 网络IO线程 1/3timeout 时间发送ping消息
    ZkClinent zkCli;
    zkCli.Start();
    // service_name 永久性节点  method_name为临时性节点
    for (auto& sp : m_serviceMap)
    {
        // service_name
        // UserServiceRpc/Login 存储当前这个rpc服务节点主机的ip的port
        std::string service_path = "/" + sp.first;
        zkCli.Create(service_path.c_str(), nullptr, 0);
        for (auto& mp : sp.second.m_methodMap)
        {
            // method_name
            std::string method_path = service_path + "/" + mp.first;
            char method_path_data[128] = {0};
            sprintf(method_path_data, "%s:%d", ip.c_str(), port);
            // ZOO_EPHEMERAL表示znode是一个临时性节点
            zkCli.Create(method_path.c_str(), method_path_data, strlen(method_path_data), ZOO_EPHEMERAL);
        }
    }
    LOG_INFO("PRCProviedr start service at ip:%s, port:%d", ip.c_str(), port);

    // 启动网络服务
    server.start();
    m_eventLoop.loop();
}

void RpcProvider::OnConnection(const TcpConnectionPtr& conn)
{
    if (!conn->connected())
    {
        // 和rpc client的连接断开
        // 关闭文件描述符
        conn->shutdown();
    }
}

/**
 * 在框架内部，RpcProvider 和 RpcConsumenr协商好之间 通信用的protobuf数据类型
 * service_name method_name args 定义proto的message类型，进行数据头的序列化和反序列化
 *                               service_name method_name args_size
 * header_size(4个字节) + header_str() + args_str
 * std::string insert 和 copy
*/
// 已建立连接用户的读写事件回调 如果远端有一个rpc服务的调用请求，OnMessage方法就会响应
void RpcProvider::OnMessage(const TcpConnectionPtr& conn, Buffer* buffer, Timestamp timestamp)
{
    // 网络上接收的远程rpc调用请求的字符流， Login args 
    std::string recv_buf = buffer->retrieveAllAsString();
    
    // 从字符流读取前4个字节的内容
    uint32_t header_size = 0;
    recv_buf.copy((char*)&header_size, 4, 0);

    LOG_INFO("rpcprovider: recv_buf:%s", recv_buf.c_str());

    // 根据header_size读取数据头的原始字节流, 反序列化数据，得到rpc请求的详细信息
    std::string rpc_header_str = recv_buf.substr(4, header_size);
    LOG_INFO("rpcprovider: rpc_header_str:%s", rpc_header_str.c_str());
    // std::cout << "rpcprovider: rpc_header_str = " << rpc_header_str << std::endl; 
    mprpc::RpcHeader rpcHeader;
    std::string service_name;   
    std::string method_name;
    uint32_t args_size;
    if (rpcHeader.ParseFromString(rpc_header_str))
    {
        // 数据头反序列化成功
        service_name = rpcHeader.service_name();
        method_name = rpcHeader.method_name();
        args_size = rpcHeader.args_size();
    }
    else
    {
        // 数据头反序列化失败
        LOG_ERROR("rpc_header_str:%s parse error!", rpc_header_str.c_str());
        return; 
    }

    
    std::string args_str = recv_buf.substr(4 + header_size, args_size);
    LOG_INFO("================================");
    LOG_INFO("recv: header_size: %d", header_size);
    LOG_INFO("recv: rpc_header_str: %s", rpc_header_str.c_str());
    LOG_INFO("recv: service_name: %s", service_name.c_str());
    LOG_INFO("recv: method_name: %s", method_name.c_str());
    LOG_INFO("recv: args_size: %d", args_size);
    LOG_INFO("recv: args_str: %s", args_str.c_str());
    LOG_INFO("================================");

    // 获取service对象和method对象
    auto it = m_serviceMap.find(service_name);
    if (it == m_serviceMap.end())
    {
        LOG_ERROR("service_name: %s is not exist", service_name.c_str());
        conn->shutdown();
        return;
    }

    auto mit = it->second.m_methodMap.find(method_name);
    if (mit == it->second.m_methodMap.end())
    {
        LOG_ERROR("method_name: %s is not exist", method_name.c_str());
        conn->shutdown();
    }

    google::protobuf::Service* service = it->second.m_service; // 获取service对象 new UserService
 
    const google::protobuf::MethodDescriptor* method = mit->second; // 获取method对象 Login

    // 生成rpc方法调用的请求request和响应response参数
    google::protobuf::Message* request = service->GetRequestPrototype(method).New();
    if (!request->ParseFromString(args_str))
    {
        LOG_ERROR("request parse error! content:%s !", args_str.c_str());
        return;
    }
    google::protobuf::Message* response =service->GetResponsePrototype(method).New();

    // 给下面的method方法的调用，绑定一个Closure的回调函数
    google::protobuf::Closure* done = google::protobuf::NewCallback<RpcProvider, const TcpConnectionPtr&, google::protobuf::Message*>(this, &RpcProvider::SendRpcResponse, conn, response);  

    // 在框架上根据远端rpc请求，调用当前rpc节点上发布的方法
    service->CallMethod(method, nullptr, request, response, done); // new UserService().Login(controller, request, response, done)
}

// Closure的回调操作，用于序列化rpc的响应的网络发送
void RpcProvider::SendRpcResponse(const TcpConnectionPtr& conn, google::protobuf::Message* response)
{
    std::string response_str;
    if (response->SerializeToString(&response_str)) // response进行序列化
    {
        // 序列化成功后，通过网络把rpc方法执行的结果发送回rpc的调用方
        conn->send(response_str);
    } 
    else
    {
        LOG_ERROR("serialize response_str error!");
    }

    conn->shutdown(); // 模拟http的短链接服务，rpcprovider主动断开连接
}