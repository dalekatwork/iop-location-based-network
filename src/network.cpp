#include <functional>

#include <easylogging++.h>

#include "network.hpp"

using namespace std;
using namespace asio::ip;



namespace LocNet
{


static const size_t ThreadPoolSize = 1;
static const size_t MessageHeaderSize = 5;
static const size_t MessageSizeOffset = 1;
static const size_t MaxMessageSize = 1024 * 1024;



TcpNetwork::TcpNetwork(const NetworkInterface &listenOn,
                       shared_ptr<IProtoBufRequestDispatcher> dispatcher) :
    _shutdownRequested(false),  _ioService(), _threadPool(),
    _keepThreadPoolBusy( new asio::io_service::work(_ioService) ),
    _acceptor( _ioService, tcp::endpoint( make_address( listenOn.address() ), listenOn.port() ) ),
    _dispatcher(dispatcher)
{
    // Switch the acceptor to listening state
    LOG(DEBUG) << "Start accepting connections";
    _acceptor.listen();
    
    shared_ptr<tcp::socket> socket( new tcp::socket(_ioService) );
    _acceptor.async_accept( *socket,
        [this, socket] (const asio::error_code &ec) { AsyncAcceptHandler(socket, ec); } );
    
    // Start the specified number of job processor threads
    for (size_t idx = 0; idx < ThreadPoolSize; ++idx)
    {
        _threadPool.push_back( thread(
            [this] { _ioService.run(); } ) );
    }
}


TcpNetwork::~TcpNetwork()
{
    // Release lock that disables return from io_service.run() even if job queue is empty
    _keepThreadPoolBusy.reset();
    
    _ioService.stop();
    for (auto &thr : _threadPool)
        { thr.join(); }
}


void TcpNetwork::Shutdown()
{
    _shutdownRequested = true;
}



void TcpNetwork::AsyncAcceptHandler(std::shared_ptr<asio::ip::tcp::socket> socket,
                                    const asio::error_code &ec)
{
    if (ec)
    {
        LOG(ERROR) << "Failed to accept connection: " << ec;
        return;
    }
    LOG(DEBUG) << "Connection accepted";
    
    // Keep accepting connections on the socket
    shared_ptr<tcp::socket> nextSocket( new tcp::socket(_ioService) );
    _acceptor.async_accept( *nextSocket,
        [this, nextSocket] (const asio::error_code &ec) { AsyncAcceptHandler(nextSocket, ec); } );
    
    // Serve connected client on separate thread
    thread serveSessionThread( [this, socket] // copy by value to keep socket alive
    {
        try
        {
            ProtoBufTcpStreamSession session(*socket);

            while (! _shutdownRequested)
            {
                LOG(INFO) << "Reading request";
                shared_ptr<iop::locnet::MessageWithHeader> requestMsg( session.ReceiveMessage() );
                if (! requestMsg)
                    { break; }
                
                LOG(INFO) << "Serving request";
                unique_ptr<iop::locnet::Response> response( _dispatcher->Dispatch( requestMsg->body().request() ) );
                
                LOG(INFO) << "Sending response";
                iop::locnet::MessageWithHeader responseMsg;
                responseMsg.mutable_body()->set_allocated_response( response.release() );
                session.SendMessage(responseMsg);
            }
        }
        catch (exception &ex)
        {
            LOG(ERROR) << "Session failed: " << ex.what();
        }
    } );
    
    // Keep thread running independently, don't block io_service here by joining it
    serveSessionThread.detach();
}




ProtoBufTcpStreamSession::ProtoBufTcpStreamSession(tcp::socket &socket) :
    _stream()
{
    //_stream.rdbuf()->assign( tcp::v4(), socket.native_handle() );
    _stream.rdbuf()->assign( socket.local_endpoint().protocol(), socket.native_handle() );
}


ProtoBufTcpStreamSession::ProtoBufTcpStreamSession(const NetworkInterface &contact) :
    _stream( contact.address(), to_string( contact.port() ) )
{
    if (! _stream)
        { throw runtime_error("Session failed to connect: " + _stream.error().message() ); }
    LOG(DEBUG) << "Connected to " << contact;
}

ProtoBufTcpStreamSession::~ProtoBufTcpStreamSession()
{
    LOG(DEBUG) << "Session closed";
}



uint32_t GetMessageSizeFromHeader(const char *bytes)
{
    // Adapt big endian value from network to local format
    const uint8_t *data = reinterpret_cast<const uint8_t*>(bytes);
    return data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
}



iop::locnet::MessageWithHeader* ProtoBufTcpStreamSession::ReceiveMessage()
{
    // Allocate a buffer for the message header and read it
    string messageBytes(MessageHeaderSize, 0);
    _stream.read( &messageBytes[0], MessageHeaderSize );
    if (! _stream.good())
        { return nullptr; }
    
    // Extract message size from the header to know how many bytes to read
    uint32_t bodySize = GetMessageSizeFromHeader( &messageBytes[MessageSizeOffset] );
    
    if (bodySize > MaxMessageSize)
        { throw runtime_error( "Message size is over limit: " + to_string(bodySize) ); }
    
    // Extend buffer to fit remaining message size and read it
    messageBytes.resize(MessageHeaderSize + bodySize, 0);
    _stream.read( &messageBytes[0] + MessageHeaderSize, bodySize );

    // Deserialize message from receive buffer, avoid leaks for failing cases with RAII-based unique_ptr
    unique_ptr<iop::locnet::MessageWithHeader> message( new iop::locnet::MessageWithHeader() );
    message->ParseFromString(messageBytes);
    return message.release();
}



void ProtoBufTcpStreamSession::SendMessage(iop::locnet::MessageWithHeader& message)
{
    message.set_header(1);
    message.set_header( message.ByteSize() - MessageHeaderSize );
    _stream << message.SerializeAsString();
}


// bool ProtoBufTcpStreamSession::IsAlive() const
// {
//     This doesn't really seem to work as on "normal" std::streamss
//     return static_cast<bool>(_stream);
// }


void ProtoBufTcpStreamSession::Close()
{
    _stream.close();
}



ProtoBufRequestNetworkDispatcher::ProtoBufRequestNetworkDispatcher(shared_ptr<IProtoBufNetworkSession> session) :
    _session(session) {}

    

unique_ptr<iop::locnet::Response> ProtoBufRequestNetworkDispatcher::Dispatch(const iop::locnet::Request& request)
{
    iop::locnet::Request *clonedReq = new iop::locnet::Request(request);
    clonedReq->set_version("1");
    
    iop::locnet::MessageWithHeader reqMsg;
    reqMsg.mutable_body()->set_allocated_request(clonedReq);
    
    _session->SendMessage(reqMsg);
    unique_ptr<iop::locnet::MessageWithHeader> respMsg( _session->ReceiveMessage() );
    if ( ! respMsg || ! respMsg->has_body() || ! respMsg->body().has_response() )
        { throw runtime_error("Got invalid response from remote node"); }
        
    unique_ptr<iop::locnet::Response> result(
        new iop::locnet::Response( respMsg->body().response() ) );
    return result;
}



// SyncTcpNodeConnectionFactory::SyncTcpNodeConnectionFactory(TcpNetwork& network) :
//     _network(network) {}


shared_ptr<INodeMethods> TcpStreamConnectionFactory::ConnectTo(const NodeProfile& node)
{
    LOG(DEBUG) << "Connecting to " << node;
    shared_ptr<IProtoBufNetworkSession> session( new ProtoBufTcpStreamSession( node.contact() ) );
    shared_ptr<IProtoBufRequestDispatcher> dispatcher( new ProtoBufRequestNetworkDispatcher(session) );
    shared_ptr<INodeMethods> result( new NodeMethodsProtoBufClient(dispatcher) );
    return result;
}



} // namespace LocNet