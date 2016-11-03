#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_set>

#include "easylogging++.h"
#include "locnet.hpp"

using namespace std;



namespace LocNet
{


static const Distance NEIGHBOURHOOD_MAX_RANGE_KM          = 100;
static const size_t   NEIGHBOURHOOD_MAX_NODE_COUNT        = 100;

static const size_t   INIT_WORLD_RANDOM_NODE_COUNT        = 100;
static const float    INIT_WORLD_NODE_FILL_TARGET_RATE    = 0.75;
static const size_t   INIT_NEIGHBOURHOOD_QUERY_NODE_COUNT = 10;




const vector<NodeInfo> Node::_seedNodes {
    // TODO put some real seed nodes in here
    NodeInfo( NodeProfile( NodeId("FirstSeedNodeId"), "1.2.3.4", 5555, "", 0 ),  GpsLocation(1.0, 2.0) ),
    NodeInfo( NodeProfile( NodeId("SecondSeedNodeId"), "6.7.8.9", 5555, "", 0 ), GpsLocation(3.0, 4.0) ),
};

random_device Node::_randomDevice;


Node::Node( const NodeInfo &nodeInfo,
            shared_ptr<ISpatialDatabase> spatialDb,
            std::shared_ptr<IRemoteNodeConnectionFactory> connectionFactory,
            bool ignoreDiscovery ) :
    _myNodeInfo(nodeInfo), _spatialDb(spatialDb), _connectionFactory(connectionFactory)
{
    if (spatialDb == nullptr) {
        throw new runtime_error("Invalid spatial database argument");
    }
    if (connectionFactory == nullptr) {
        throw new runtime_error("Invalid connection factory argument");
    }
    
    if ( _spatialDb->GetNodeCount(NodeRelationType::Colleague) == 0 && ! ignoreDiscovery )
    {
        bool discoverySucceeded = DiscoverWorld() && DiscoverNeighbourhood();
        if (! discoverySucceeded)
            { throw new runtime_error("Network discovery failed"); }
    }
}


const unordered_map<ServiceType,ServiceProfile,EnumHasher>& Node::services() const
    { return _services; }
    
void Node::RegisterService(ServiceType serviceType, const ServiceProfile& serviceInfo)
{
    auto it = _services.find(serviceType);
    if ( it != _services.end() ) {
        throw runtime_error("Service type is already registered");
    }
    _services[serviceType] = serviceInfo;
}

void Node::RemoveService(ServiceType serviceType)
{
    auto it = _services.find(serviceType);
    if ( it == _services.end() ) {
        throw runtime_error("Service type was not registered");
    }
    _services.erase(serviceType);
}


bool Node::AcceptColleague(const NodeInfo &newNode)
{
    return SafeStoreNode( NodeDbEntry(
        newNode, NodeRelationType::Colleague, NodeContactRoleType::Acceptor) );
}


bool Node::RenewNodeConnection(const NodeInfo &updatedNode)
{
    shared_ptr<NodeInfo> storedProfile = _spatialDb->Load( updatedNode.profile().id() );
    if (storedProfile != nullptr) {
        if ( storedProfile->location() == updatedNode.location() ) {
            return _spatialDb->Update(updatedNode);
        }
        // TODO consider how we should handle changed location here: recalculate bubbles or simply deny renewal
    }
    return false;
}


bool Node::AcceptNeighbour(const NodeInfo &node)
{
    return SafeStoreNode( NodeDbEntry(
        node, NodeRelationType::Neighbour, NodeContactRoleType::Acceptor) );
}



size_t Node::GetNodeCount(NodeRelationType nodeType) const
    { return _spatialDb->GetNodeCount(nodeType); }

Distance Node::GetNeighbourhoodRadiusKm() const
    { return _spatialDb->GetNeighbourhoodRadiusKm(); }

vector<NodeInfo> Node::GetRandomNodes(
    size_t maxNodeCount, Neighbours filter) const
{
    return _spatialDb->GetRandomNodes(maxNodeCount, filter);
}

vector<NodeInfo> Node::GetClosestNodes(const GpsLocation& location,
    Distance radiusKm, size_t maxNodeCount, Neighbours filter) const
{
    return _spatialDb->GetClosestNodes(location, radiusKm, maxNodeCount, filter);
}


Distance Node::GetBubbleSize(const GpsLocation& location) const
{
    Distance distance = _spatialDb->GetDistanceKm( _myNodeInfo.location(), location );
    Distance bubbleSize = log10(distance + 2500.) * 500. - 1700.;
    return bubbleSize;
}


bool Node::BubbleOverlaps(const GpsLocation& newNodeLocation) const
{
    // Get our closest node to location, no matter the radius
    vector<NodeInfo> closestNodes = _spatialDb->GetClosestNodes(
        newNodeLocation, numeric_limits<Distance>::max(), 1, Neighbours::Excluded);
    
    // If there is no such point yet (i.e. map is still empty), it cannot overlap
    if ( closestNodes.empty() ) { return false; }
    
    // Get bubble sizes of both locations
    const GpsLocation &myClosesNodeLocation = closestNodes.front().location();
    Distance myClosestNodeBubbleSize = GetBubbleSize(myClosesNodeLocation);
    Distance newNodeBubbleSize       = GetBubbleSize(newNodeLocation);
    
    // If sum of bubble sizes greater than distance of points, the bubbles overlap
    Distance newNodeDistanceFromClosestNode = _spatialDb->GetDistanceKm(newNodeLocation, myClosesNodeLocation);
    return myClosestNodeBubbleSize + newNodeBubbleSize > newNodeDistanceFromClosestNode;
}


shared_ptr<IRemoteNode> Node::SafeConnectTo(const NodeProfile& node)
{
    // There is no point in connecting to ourselves
    if ( node.id() == _myNodeInfo.profile().id() )
        { return shared_ptr<IRemoteNode>(); }
    
    try { return _connectionFactory->ConnectTo(node); }
    catch (exception &e)
    {
        LOG(WARNING) << "Failed to connect to " << node.ipv4Address() << ":" << node.ipv4Port() << " / "
                                                << node.ipv6Address() << ":" << node.ipv6Port() << endl;
        LOG(WARNING) << "Error was: " << e.what() << endl;
        return shared_ptr<IRemoteNode>();
    }
}


bool Node::SafeStoreNode(const NodeDbEntry& entry,
    shared_ptr<IRemoteNode> nodeConnection)
{
    try
    {
        // Check if node is acceptable
        switch ( entry.relationType() )
        {
            case NodeRelationType::Neighbour:
                if ( _spatialDb->GetNodeCount(NodeRelationType::Neighbour) >= NEIGHBOURHOOD_MAX_NODE_COUNT ||
                     _spatialDb->GetDistanceKm( _myNodeInfo.location(), entry.location() ) >= NEIGHBOURHOOD_MAX_RANGE_KM )
                    { return false; }
                break;
                
            case NodeRelationType::Colleague:
                if ( BubbleOverlaps( entry.location() ) )
                    { return false; }
                break;
                
            default:
                throw runtime_error("Unknown nodetype, missing implementation");
        }
        
        if ( entry.roleType() == NodeContactRoleType::Initiator )
        {
            // If no connection argument is specified, try connecting to candidate node
            if (nodeConnection == nullptr)
                { nodeConnection = SafeConnectTo( entry.profile() ); }
            if (nodeConnection == nullptr)
                { return false; }
            
            // Ask for its permission to add it
            switch ( entry.relationType() )
            {
                case NodeRelationType::Colleague:
                    if ( ! nodeConnection->AcceptColleague(_myNodeInfo) )
                        { return false; }
                    break;
                    
                case NodeRelationType::Neighbour:
                    if ( ! nodeConnection->AcceptNeighbour(_myNodeInfo) )
                        { return false; }
                    break;
                    
                default:
                    throw runtime_error("Unknown nodetype, missing implementation");
            }
        }
        
        return _spatialDb->Store(entry);
    }
    catch (exception &e)
    {
        LOG(WARNING) << "Unexpected error storing node: " << e.what() << endl;
    }
    
    return false;
}


// TODO consider if this is guaranteed to stop
bool Node::DiscoverWorld()
{
    // Initialize random generator and utility variables
    uniform_int_distribution<int> fromRange( 0, _seedNodes.size() - 1 );
    vector<string> triedSeedNodes;
    
    size_t seedNodeColleagueCount = 0;
    vector<NodeInfo> randomColleagueCandidates;
    while ( triedSeedNodes.size() < _seedNodes.size() )
    {
        // Select random node from hardwired seed node list
        size_t selectedSeedNodeIdx = fromRange(_randomDevice);
        const NodeInfo& selectedSeedNode = _seedNodes[selectedSeedNodeIdx];
        
        // If node has been already tried and failed, choose another one
        auto it = find( triedSeedNodes.begin(), triedSeedNodes.end(), selectedSeedNode.profile().id() );
        if ( it != triedSeedNodes.end() )
            { continue; }
        
        try
        {
            triedSeedNodes.push_back( selectedSeedNode.profile().id() );
            
            // Try connecting to selected seed node
            shared_ptr<IRemoteNode> seedNodeConnection = SafeConnectTo( selectedSeedNode.profile() );
            if (seedNodeConnection == nullptr)
                { continue; }
            
            // And query both a target World Map size and an initial list of random nodes to start with
            seedNodeColleagueCount = seedNodeConnection->GetNodeCount(NodeRelationType::Colleague);
            randomColleagueCandidates = seedNodeConnection->GetRandomNodes(
                min(INIT_WORLD_RANDOM_NODE_COUNT, seedNodeColleagueCount), Neighbours::Excluded );
            
            // If got a reasonable response from a seed server, stop contacting other seeds
            if ( seedNodeColleagueCount > 0 && ! randomColleagueCandidates.empty() )
            {
                // Try to add seed node to our network and step out of seed node phase
                Distance seedNodeDistance = _spatialDb->GetDistanceKm( _myNodeInfo.location(), selectedSeedNode.location() );
                NodeRelationType seedNodeType = seedNodeDistance <= NEIGHBOURHOOD_MAX_RANGE_KM ?
                    NodeRelationType::Neighbour : NodeRelationType::Colleague;
                SafeStoreNode( NodeDbEntry(selectedSeedNode, seedNodeType, NodeContactRoleType::Initiator) );
                break;
            }
        }
        catch (exception &e)
        {
            LOG(WARNING) << "Failed to connect to seed node: " << e.what() << ", trying other seeds" << endl;
        }
    }
    
    // Check if all seed nodes tried and failed
    if ( seedNodeColleagueCount == 0 && randomColleagueCandidates.empty() &&
         triedSeedNodes.size() == _seedNodes.size() )
    {
        // This still might be normal if we're the very first seed node of the whole network
        auto seedIt = find_if( _seedNodes.begin(), _seedNodes.end(),
            [this] (const NodeInfo& seedProfile) { return _myNodeInfo.profile() == seedProfile.profile(); } );
        if ( seedIt == _seedNodes.end() )
        {
            // TODO reconsider error handling here, should we completely give up and maybe exit()?
            LOG(ERROR) << "All seed nodes have been tried and failed, giving up" << endl;
            return false;
        }
    }
    
    // We received a reasonable random node list from a seed, try to fill in our world map
    size_t targetColleageCound = INIT_WORLD_NODE_FILL_TARGET_RATE * seedNodeColleagueCount;
    for (size_t addedColleagueCount = 0; addedColleagueCount < targetColleageCound; )
    {
        if ( ! randomColleagueCandidates.empty() )
        {
            // Pick a single node from the candidate list and try to make it a colleague node
            NodeInfo nodeInfo( randomColleagueCandidates.back() );
            randomColleagueCandidates.pop_back();
            
            if ( SafeStoreNode( NodeDbEntry(nodeInfo, NodeRelationType::Colleague, NodeContactRoleType::Initiator) ) )
                { ++addedColleagueCount; }
        }
        else
        {
            // We ran out of colleague candidates, pick some more random candidates
            while ( randomColleagueCandidates.empty() )
            {
                // Select random node that we already know so far
                randomColleagueCandidates = _spatialDb->GetRandomNodes(1, Neighbours::Excluded);
                if ( randomColleagueCandidates.empty() )
                {
                    // TODO reconsider error handling here
                    cerr << "After trying all random nodes returned by seed, still have no colleagues, give up" << endl;
                    return false;
                }
                
                try
                {
                    // Connect to selected random node
                    shared_ptr<IRemoteNode> randomConnection = SafeConnectTo( randomColleagueCandidates.front().profile() );
                    if (randomConnection == nullptr)
                        { continue; }
                    
                    // Ask for random colleague candidates
                    randomColleagueCandidates = randomConnection->GetRandomNodes(
                        INIT_WORLD_RANDOM_NODE_COUNT, Neighbours::Excluded);
                }
                catch (exception &e)
                {
                    LOG(WARNING) << "Failed to fetch more random nodes: " << e.what() << endl;
                }
            }
        }
    }
    
    return true;
}



bool Node::DiscoverNeighbourhood()
{
    // Get the closest node known to us so far
    vector<NodeInfo> newClosestNode = _spatialDb->GetClosestNodes(
        _myNodeInfo.location(), numeric_limits<Distance>::max(), 1, Neighbours::Included);
    if ( newClosestNode.empty() )
        { return false; }
    
    // Repeat asking (so far) closest node for an even closer node until no new node discovered
    vector<NodeInfo> oldClosestNode;
    while ( oldClosestNode.size() != newClosestNode.size() ||
            oldClosestNode.front().profile().id() != newClosestNode.front().profile().id() )
    {
        oldClosestNode = newClosestNode;
        try
        {
            shared_ptr<IRemoteNode> closestNodeConnection =
                SafeConnectTo( newClosestNode.front().profile() );
            if (closestNodeConnection == nullptr) {
                // TODO what to do if closest node is not reachable?
                return false;
            }
            
            newClosestNode = closestNodeConnection->GetClosestNodes(
                _myNodeInfo.location(), numeric_limits<Distance>::max(), 1, Neighbours::Included);
        }
        catch (exception &e) {
            LOG(WARNING) << "Failed to fetch neighbours: " << e.what() << endl;
            // TODO consider what else to do here
        }
    }
    
    deque<NodeInfo> nodesToAskQueue( newClosestNode.begin(), newClosestNode.end() );
    unordered_set<string> askedNodeIds;
    
    // Try to fill neighbourhood map until limit reached or no new nodes left to ask
    vector<NodeInfo> neighbourCandidates;
    while ( neighbourCandidates.size() < NEIGHBOURHOOD_MAX_NODE_COUNT &&
            ! nodesToAskQueue.empty() )
    {
        // Get next candidate
        NodeInfo neighbourCandidate = nodesToAskQueue.front();
        nodesToAskQueue.pop_front();
        
        // Skip it if has been processed already
        auto askedIt = find( askedNodeIds.begin(), askedNodeIds.end(), neighbourCandidate.profile().id() );
        if ( askedIt != askedNodeIds.end() )
            { continue; }
            
        try
        {
            // Try connecting to the node
            shared_ptr<IRemoteNode> candidateConnection = SafeConnectTo( neighbourCandidate.profile() );
            if (candidateConnection == nullptr)
                { continue; }
                
            // Try to add node as neighbour, reusing connection
            SafeStoreNode( NodeDbEntry(neighbourCandidate, NodeRelationType::Neighbour, NodeContactRoleType::Initiator), candidateConnection );
            
            // Get its neighbours closest to us
            vector<NodeInfo> newNeighbourCandidates = candidateConnection->GetClosestNodes(
                _myNodeInfo.location(), INIT_NEIGHBOURHOOD_QUERY_NODE_COUNT, 10, Neighbours::Included );
            
            // Mark current node as processed and append its new neighbours to our todo list
            askedNodeIds.insert( neighbourCandidate.profile().id() );
            nodesToAskQueue.insert( nodesToAskQueue.end(),
                newNeighbourCandidates.begin(), newNeighbourCandidates.end() );
        }
        catch (exception &e) {
            LOG(WARNING) << "Failed to add neighbour node: " << e.what() << endl;
            // TODO consider what else to do here?
        }
    }
    
    return true;
}


} // namespace LocNet