
#include <array>
#include <functional>
#include <iostream>
#include <boost/asio.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/thread/thread.hpp>
#include <unordered_map>
#include <numeric>
#include <list>

using namespace std;
using namespace boost::asio;
using namespace boost::asio::ip;
using namespace boost;

const int MAX_IP_PACK_SIZE = 65536;
const int HEAD_LEN = 4;

class Message
{
public:
    enum{header_length = 4};
    enum{max_body_length = 512};

    Message():body_length_(0)
    {

    }
    const char* data() const
    {
        return data_;
    }
    char* data()
    {
        return data_;
    }
    size_t length() const
    {
        return header_length + body_length_;
    }
    const char* body() const
    {
        return data_ + header_length;
    }
    char* body()
    {
        return data_ + header_length;
    }
    size_t body_length() const
    {
        return body_length_;
    }
    void body_length(size_t new_length)
    {
        body_length_ = new_length;
        if(body_length_>max_body_length)
        {
            body_length_ = max_body_length;
        }
    }
    bool decode_header()
    {
        char header[header_length+1] = "";
        std::strncat(header,data_,header_length);
        body_length_ = std::atoi(header) - header_length;
        if(body_length_>max_body_length)
        {
            body_length_ = 0;
            return false;
        }
        return true;
    }
    void encode_header()
    {
        char header[header_length + 1] = "";
        std::sprintf(header,"%4d",body_length_);
        std::memcpy(data_,header,header_length);

    }

private:
    char data_[header_length + max_body_length];
    std::size_t body_length_;


};


class RWHandler:public std::enable_shared_from_this<RWHandler>
{
public:
    RWHandler(io_service& ios):m_sock(ios)
    {

    }
    ~RWHandler()
    {

    }

    void HandleRead()
    {
        auto self = shared_from_this();

        async_read(m_sock,buffer(m_readMsg.data(),HEAD_LEN),[this,self](const boost::system::error_code& ec,size_t size)
        {
//            if(ec != nullptr)
            if(ec || !m_readMsg.decode_header())
            {
                HandleError(ec);
                return;
            }

            ReadBody();
        });
    }
    void ReadBody()
    {
        auto self = shared_from_this();
        async_read(m_sock,buffer(m_readMsg.body(),m_readMsg.body_length()),[this,self](const boost::system::error_code& ec,size_t size)
        {
            if(ec)
            {
                HandleError(ec);
                return;
            }
            CallBack(m_readMsg.data(),m_readMsg.length());
            HandleRead();
        });
    }
    void HandleWrite(char* data,int len)
    {
        boost::system::error_code ec;
        write(m_sock,buffer(data,len),ec);
//        if(ec != nullptr)
        if(ec)
        {
            HandleError(ec);
        }
    }

    tcp::socket& GetSocket()
    {
        return m_sock;
    }

    void CloseSocket()
    {
        boost::system::error_code ec;
        m_sock.shutdown(tcp::socket::shutdown_both,ec);
        m_sock.close(ec);
    }

    void SetConnId(int connId)
    {
        m_connId = connId;
    }

    int GetConnId() const
    {
        return m_connId;
    }

    template<typename F>
    void SetCallBackError(F f)
    {
        m_callbackError = f;
    }
    void CallBack(char* pData,int len)
    {
        cout<<pData + HEAD_LEN<<endl;
    }

private:
    void HandleError(const boost::system::error_code& ec)
    {
        CloseSocket();
        cout<<ec.message()<<endl;
        if(m_callbackError)
        {
            m_callbackError(m_connId);
        }
    }

private:
    tcp::socket m_sock;
//    std::array<char,MAX_IP_PACK_SIZE> m_buff;
    int m_connId;
    std::function<void(int)> m_callbackError;
    Message m_readMsg;


};


const int MaxConnectionNum = 65536;
const int MaxRecvSize = 65536;
class Server
{
public:
    Server(io_service& ios,short port):m_ios(ios),m_acceptor(ios,tcp::endpoint(tcp::v4(),port)),m_cnnIdPool(MaxConnectionNum)
    {
        m_cnnIdPool.resize(MaxConnectionNum);
        std::iota(m_cnnIdPool.begin(),m_cnnIdPool.end(),1);
    }
    ~Server()
    {

    }

    void Accept()
    {
        cout<<"Start Listening"<<endl;
        std::shared_ptr<RWHandler> handler = CreateHandler();

        m_acceptor.async_accept(handler->GetSocket(),[this,handler](const boost::system::error_code& error)
        {
            if(error)
            {
                cout<<error.value()<<" "<<error.message()<<endl;
                HandleAcpError(handler,error);
                return;
            }
            m_handles.insert(std::make_pair(handler->GetConnId(),handler));
            cout<<"current connect count: "<<m_handles.size()<<endl;
            handler->HandleRead();
            Accept();
        });
    }

private:
    void HandleAcpError(std::shared_ptr<RWHandler> eventHandler,const boost::system::error_code& error)
    {
        cout<<"Error, error reason: "<<error.value()<<error.message()<<endl;

        eventHandler->CloseSocket();
        StopAccept();
    }
    void StopAccept()
    {
        boost::system::error_code ec;
        m_acceptor.cancel(ec);
        m_acceptor.close(ec);
        m_ios.stop();
    }

    std::shared_ptr<RWHandler> CreateHandler()
    {
        int connId = m_cnnIdPool.front();
        m_cnnIdPool.pop_front();
        std::shared_ptr<RWHandler> handler = std::make_shared<RWHandler>(m_ios);

        handler->SetConnId(connId);

        handler->SetCallBackError([this](int connId)
                                  {
                                      RecyclConnid(connId);
                                  });

        return handler;
    }

    void RecyclConnid(int connId)
    {
        auto it = m_handles.find(connId);
        if(it!= m_handles.end())
        {
            m_handles.erase(it);
        }
        cout<<"current connect count: "<<m_handles.size()<<endl;
        m_cnnIdPool.push_back(connId);
    }

private:
    io_service& m_ios;
    tcp::acceptor m_acceptor;
    std::unordered_map<int,std::shared_ptr<RWHandler>> m_handles;
    list<int> m_cnnIdPool;
};



class Connector
{
public:
    Connector(io_service& ios,const string& strIP,short port):m_ios(ios),m_socket(ios),m_serverAddr(tcp::endpoint(address::from_string(strIP),port)),m_isConnected(false),m_chkThread(nullptr)
    {
        CreateEventHandler(ios);
    }

    ~Connector()
    {

    }

    bool Start()
    {
        m_eventHandler->GetSocket().async_connect(m_serverAddr,[this](const boost::system::error_code& error)
        {
            if(error)
            {
                HandleConnectError(error);
                return;

            }
            cout<<"connect ok"<<endl;
            m_isConnected = true;
            m_eventHandler->HandleRead();
        });

        boost::this_thread::sleep(boost::posix_time::seconds(1));
        return m_isConnected;
    }

    bool IsConnected() const
    {
        return m_isConnected;

    }

    void Send(char* data,int len)
    {
        if(!m_isConnected)
        {
            return;
        }
        m_eventHandler->HandleWrite(data,len);
    }

private:
    void CreateEventHandler(io_service& ios)
    {
        m_eventHandler = std::make_shared<RWHandler>(ios);
        m_eventHandler->SetCallBackError([this](int connid)
                                         {
                                             HandleRWError(connid);
                                         });
    }

    void CheckConnect()
    {
        if(m_chkThread!= nullptr)
        {
            return;

        }

        m_chkThread= std::make_shared<std::thread>([this]
                                                   {
                                                       while(true)
                                                       {
                                                           if(!IsConnected())
                                                           {
                                                               Start();
                                                           }

                                                           boost::this_thread::sleep(boost::posix_time::seconds(1));
                                                       }
                                                   });
    }


    void HandleConnectError(const boost::system::error_code& error)
    {
        m_isConnected = false;
        cout<<error.message()<<endl;
        m_eventHandler->CloseSocket();
        CheckConnect();
    }

    void HandleRWError(int connid)
    {
        m_isConnected = false;
        CheckConnect();
    }
private:
    io_service& m_ios;
    tcp::socket m_socket;
    tcp::endpoint m_serverAddr;

    std::shared_ptr<RWHandler> m_eventHandler;
    bool m_isConnected;
    std::shared_ptr<std::thread> m_chkThread;
};




