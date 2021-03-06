#include <iostream> // TODO remove if not needed
#include <map>
#include <algorithm>
#include "thor/isochrone.h"
#include "baldr/datetime.h"
#include "midgard/distanceapproximator.h"
#include "midgard/logging.h"

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;

namespace {

// Method to get an operator Id from a map of operator strings vs. Id.
uint32_t GetOperatorId(const GraphTile* tile, uint32_t routeid,
            std::unordered_map<std::string, uint32_t>& operators) {
  const TransitRoute* transit_route = tile->GetTransitRoute(routeid);

  // Test if the transit operator changed
  if (transit_route && transit_route->op_by_onestop_id_offset()) {
    // Get the operator name and look up in the operators map
    std::string operator_name =
        tile->GetName(transit_route->op_by_onestop_id_offset());
    auto operator_itr = operators.find(operator_name);
    if (operator_itr == operators.end()) {
      // Operator not found - add to the map
      uint32_t id = operators.size() + 1;
      operators[operator_name] = id;
      return id;
    } else {
      return operator_itr->second;
    }
  }
  return 0;
}

constexpr float to_minutes = 1.0f / 60.0f;

}

namespace valhalla {
namespace thor {

constexpr uint32_t kBucketCount = 20000;
constexpr uint64_t kInitialEdgeLabelCount = 500000;

// Default constructor
Isochrone::Isochrone()
    : access_mode_(kAutoAccess),
      tile_creation_date_(0),
      shape_interval_(50.0f),
      mode_(TravelMode::kDrive),
      adjacencylist_(nullptr),
      edgestatus_(nullptr) {
}

// Destructor
Isochrone::~Isochrone() {
  Clear();
}

// Clear the temporary information generated during path construction.
void Isochrone::Clear() {
  // Clear the edge labels, edge status flags, and adjacency list
  edgelabels_.clear();
  adjacencylist_.reset();
  edgestatus_.reset();
}

// Construct the isotile. Use a grid size based on travel mode.
// Convert time in minutes to a max distance in meters based on an
// estimate of max average speed for the travel mode.
void Isochrone::ConstructIsoTile(const bool multimodal, const unsigned int max_minutes,
                                 std::vector<baldr::PathLocation>& origin_locations) {
  float grid_size, max_distance;
  auto max_seconds = max_minutes * 60;
  if (multimodal) {
    grid_size = 200.0f;
    max_distance = max_seconds * 70.0f * 0.44704f; // TODO
  } else if (mode_ == TravelMode::kPedestrian) {
    grid_size = 200.0f;
    max_distance = max_seconds * 5.0f * 0.44704f;
  } else if (mode_ == TravelMode::kBicycle) {
    grid_size = 200.0f;
    max_distance = max_seconds * 20.0f * 0.44704f;
  } else {
    // A driving mode
    grid_size = 400.0f;
    max_distance = max_seconds * 70.0f * 0.44704f;
  }
  shape_interval_ = grid_size * 0.25f;

  // Form grid for isotiles. Convert grid size to degrees.
  grid_size /= kMetersPerDegreeLat;
  float lat = origin_locations[0].latlng_.lat();
  float dlat = max_distance / kMetersPerDegreeLat;
  float dlon = max_distance / DistanceApproximator::MetersPerLngDegree(lat);
  AABB2<PointLL> bounds(10000.0f, 10000.0f, -10000.0f, -10000.0f);
  for (const auto& loc : origin_locations) {
    PointLL center = loc.latlng_;
    AABB2<PointLL> bbox(PointLL(center.lng() - dlon, center.lat() - dlat),
                        PointLL(center.lng() + dlon, center.lat() + dlat));
    bounds.Expand(bbox);
  }
  isotile_.reset(new GriddedData<PointLL>(bounds, grid_size, max_minutes + 5));
}

// Initialize - create adjacency list, edgestatus support, and reserve
// edgelabels
void Isochrone::Initialize(const uint32_t bucketsize) {
  edgelabels_.reserve(kInitialEdgeLabelCount);

  // Set up lambda to get sort costs
  const auto edgecost = [this](const uint32_t label) {
    return edgelabels_[label].sortcost();
  };

  float range = kBucketCount * bucketsize;
  adjacencylist_.reset(new DoubleBucketQueue(0.0f, range, bucketsize, edgecost));
  edgestatus_.reset(new EdgeStatus());
}

// Compute iso-tile that we can use to generate isochrones.
std::shared_ptr<const GriddedData<PointLL> > Isochrone::Compute(
             std::vector<PathLocation>& origin_locations,
             const unsigned int max_minutes,
             GraphReader& graphreader,
             const std::shared_ptr<DynamicCost>* mode_costing,
             const TravelMode mode) {
  // Set the mode and costing
  mode_ = mode;
  const auto& costing = mode_costing[static_cast<uint32_t>(mode_)];

  // Initialize and create the isotile
  auto max_seconds = max_minutes * 60;
  Initialize(costing->UnitSize());
  ConstructIsoTile(false, max_minutes, origin_locations);

  // Set the origin locations
  SetOriginLocations(graphreader, origin_locations, costing);

  // Compute the isotile
  uint32_t n = 0;
  const GraphTile* tile;
  while (true) {
    // Get next element from adjacency list. Check that it is valid. An
    // invalid label indicates there are no edges that can be expanded.
    uint32_t predindex = adjacencylist_->pop();
    if (predindex == kInvalidLabel) {
      return isotile_;
    }

    // Copy the EdgeLabel for use in costing and settle the edge.
    EdgeLabel pred = edgelabels_[predindex];
    edgestatus_->Update(pred.edgeid(), EdgeSet::kPermanent);

    // Get the end node of the prior directed edge. Skip if tile not found
    // (can happen with regional data sets).
    GraphId node = pred.endnode();
    if ((tile = graphreader.GetGraphTile(node)) == nullptr) {
      continue;
    }

    // Get the nodeinfo and update the isotile
    const NodeInfo* nodeinfo = tile->node(node);
    UpdateIsoTile(pred, graphreader, nodeinfo->latlng());
    n++;

    // Return after the time interval has been met
    if (pred.cost().secs > max_seconds) {
      LOG_DEBUG("Exceed time interval: n = " + std::to_string(n));
      return isotile_;
    }

    // Check access at the node
    if (!costing->Allowed(nodeinfo)) {
      continue;
    }

    // Expand from end node.
    GraphId edgeid(node.tileid(), node.level(), nodeinfo->edge_index());
    const DirectedEdge* directededge = tile->directededge(nodeinfo->edge_index());
    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, edgeid++) {
      // Skip shortcut edges
      if (directededge->is_shortcut()) {
        continue;
      }

      // Get the current set. Skip this edge if permanently labeled (best
      // path already found to this directed edge).
      EdgeStatusInfo edgestatus = edgestatus_->Get(edgeid);
      if (edgestatus.set() == EdgeSet::kPermanent) {
        continue;
      }

      // Handle transition edges - add to adjacency set
      // TODO - perhaps create ExpandForward method like in bidir A*
      if (directededge->trans_up() || directededge->trans_down()) {
        uint32_t idx = edgelabels_.size();
        adjacencylist_->add(idx, pred.sortcost());
        edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
        edgelabels_.emplace_back(predindex, edgeid, directededge->endnode(), pred);
        continue;
      }

      // Skip if no access is allowed to this edge (based on the costing
      // method) or if a complex restriction exists for this path.
      if (!costing->Allowed(directededge, pred, tile, edgeid) ||
           costing->Restricted(directededge, pred, edgelabels_, tile,
                               edgeid, true)) {
        continue;
      }

      // Compute the cost to the end of this edge
      Cost newcost = pred.cost() + costing->EdgeCost(directededge) +
			     costing->TransitionCost(directededge, nodeinfo, pred);

      // Check if edge is temporarily labeled and this path has less cost. If
      // less cost the predecessor is updated and the sort cost is decremented
      // by the difference in real cost (A* heuristic doesn't change)
      if (edgestatus.set() == EdgeSet::kTemporary) {
        CheckIfLowerCostPath(edgestatus.index(), predindex, newcost);
        continue;
      }

      // Add to the adjacency list and edge labels.
      uint32_t idx = edgelabels_.size();
      adjacencylist_->add(idx, newcost.cost);
      edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
      edgelabels_.emplace_back(predindex, edgeid, directededge,
                    newcost, newcost.cost, 0.0f, mode_, 0);
    }
  }
  return isotile_;      // Should never get here
}

// Compute iso-tile that we can use to generate isochrones.
std::shared_ptr<const GriddedData<PointLL> > Isochrone::ComputeReverse(
             std::vector<PathLocation>& dest_locations,
             const unsigned int max_minutes,
             GraphReader& graphreader,
             const std::shared_ptr<DynamicCost>* mode_costing,
             const TravelMode mode) {
  // Set the mode and costing
  mode_ = mode;
  const auto& costing = mode_costing[static_cast<uint32_t>(mode_)];
  access_mode_ = costing->access_mode();

  // Initialize and create the isotile
  auto max_seconds = max_minutes * 60;
  Initialize(costing->UnitSize());
  ConstructIsoTile(false, max_minutes, dest_locations);

  // Set the origin locations
  SetDestinationLocations(graphreader, dest_locations, costing);

  // Compute the isotile
  uint32_t n = 0;
  const GraphTile* tile;
  while (true) {
    // Get next element from adjacency list. Check that it is valid. An
    // invalid label indicates there are no edges that can be expanded.
    uint32_t predindex = adjacencylist_->pop();
    if (predindex == kInvalidLabel) {
      return isotile_;
    }

    // Copy the EdgeLabel for use in costing and settle the edge.
    EdgeLabel pred = edgelabels_[predindex];
    edgestatus_->Update(pred.edgeid(), EdgeSet::kPermanent);

    // Get the end node of the prior directed edge. Skip if tile not found
    // (can happen with regional data sets).
    GraphId node = pred.endnode();
    if ((tile = graphreader.GetGraphTile(node)) == nullptr) {
      continue;
    }

    // Get the nodeinfo and update the isotile
    const NodeInfo* nodeinfo = tile->node(node);
    UpdateIsoTile(pred, graphreader, nodeinfo->latlng());
    n++;

    // Return after the time interval has been met
    if (pred.cost().secs > max_seconds) {
      LOG_DEBUG("Exceed time interval: n = " + std::to_string(n));
      return isotile_;
    }

    // Check access at the node
    if (!costing->Allowed(nodeinfo)) {
      continue;
    }

    // Get the opposing predecessor directed edge.
    const DirectedEdge* opp_pred_edge =
        (pred.opp_edgeid().Tile_Base() == tile->id().Tile_Base()) ?
            tile->directededge(pred.opp_edgeid().id()) :
            graphreader.GetGraphTile(pred.opp_edgeid().
                  Tile_Base())->directededge(pred.opp_edgeid());

    // Expand from end node.
    GraphId edgeid(node.tileid(), node.level(), nodeinfo->edge_index());
    const DirectedEdge* directededge = tile->directededge(nodeinfo->edge_index());
    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, edgeid++) {
      // Skip edges not allowed by the access mode. This allows early rejection
      // without the opposing edge. Also skip edges shortcut edges.
      if (!(directededge->reverseaccess() & access_mode_) ||
            directededge->is_shortcut()) {
        continue;
      }

      // Get the current set. Skip this edge if permanently labeled (best
      // path already found to this directed edge).
      EdgeStatusInfo edgestatus = edgestatus_->Get(edgeid);
      if (edgestatus.set() == EdgeSet::kPermanent) {
        continue;
      }

      // Handle transition edges. Add to adjacency list.
      // TODO - perhaps create ExpandReverseMethod like in bi-dir A*
      if (directededge->trans_up() || directededge->trans_down()) {
        uint32_t idx = edgelabels_.size();
        adjacencylist_->add(idx, pred.sortcost());
        edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
        edgelabels_.emplace_back(predindex, edgeid, directededge->endnode(), pred);
        continue;
      }

      // Get opposing edge Id and end node tile
      const GraphTile* t2 = directededge->leaves_tile() ?
           graphreader.GetGraphTile(directededge->endnode()) : tile;
      if (t2 == nullptr) {
        continue;
      }
      GraphId oppedge = t2->GetOpposingEdgeId(directededge);

      // Get opposing directed edge and check if allowed.
      const DirectedEdge* opp_edge = t2->directededge(oppedge);
      if (!costing->AllowedReverse(directededge, pred, opp_edge,
                                   tile, edgeid)) {
        continue;
      }

      // Check for complex restriction
      if (costing->Restricted(directededge, pred, edgelabels_, tile,
                               edgeid, false)) {
        continue;
      }

      // Compute the cost to the end of this edge with separate transition cost
      Cost tc = costing->TransitionCostReverse(directededge->localedgeidx(),
                                  nodeinfo, opp_edge, opp_pred_edge);
      Cost newcost = pred.cost() + costing->EdgeCost(opp_edge);
      newcost.cost += tc.cost;

      // Check if edge is temporarily labeled and this path has less cost. If
      // less cost the predecessor is updated and the sort cost is decremented
      // by the difference in real cost (A* heuristic doesn't change)
      if (edgestatus.set() == EdgeSet::kTemporary) {
        CheckIfLowerCostPath(edgestatus.index(), predindex, newcost);
        continue;
      }

      // Add edge label, add to the adjacency list and set edge status
      uint32_t idx = edgelabels_.size();
      adjacencylist_->add(idx, newcost.cost);
      edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
      edgelabels_.emplace_back(predindex, edgeid, oppedge,
                    directededge, newcost, newcost.cost, 0.0f,
                    mode_, tc, false);
    }
  }
  return isotile_;      // Should never get here
}

// Compute isochrone for mulit-modal route.
std::shared_ptr<const GriddedData<PointLL> > Isochrone::ComputeMultiModal(
             std::vector<PathLocation>& origin_locations,
             const unsigned int max_minutes, GraphReader& graphreader,
             const std::shared_ptr<DynamicCost>* mode_costing,
             const TravelMode mode) {
  // For pedestrian costing - set flag allowing use of transit connections
  // Set pedestrian costing to use max distance. TODO - need for other modes
  const auto& pc = mode_costing[static_cast<uint8_t>(TravelMode::kPedestrian)];
  pc->SetAllowTransitConnections(true);
  pc->UseMaxMultiModalDistance();

  // Set the mode from the origin
  mode_ = mode;
  const auto& costing = mode_costing[static_cast<uint8_t>(mode)];
  const auto& tc = mode_costing[static_cast<uint8_t>(TravelMode::kPublicTransit)];
  bool wheelchair = tc->wheelchair();
  bool bicycle = tc->bicycle();

  // Get maximum transfer distance (TODO - want to allow unlimited walking once
  // you get off the transit stop...)
  uint32_t max_transfer_distance = 99999.0f; //costing->GetMaxTransferDistanceMM();

  // Initialize and create the isotile
  auto max_seconds = max_minutes * 60;
  Initialize(costing->UnitSize());
  ConstructIsoTile(true, max_minutes, origin_locations);

  // Set the origin locations.
  SetOriginLocations(graphreader, origin_locations, costing);

  // For now the date_time must be set on the origin.
  if (!origin_locations.front().date_time_) {
    LOG_ERROR("No date time set on the origin location");
    return isotile_;
  }

  // Update start time
  uint32_t start_time, localtime, date, dow, day = 0;
  bool date_before_tile = false;
  if (origin_locations[0].date_time_) {
    // Set route start time (seconds from midnight), date, and day of week
    start_time = DateTime::seconds_from_midnight(*origin_locations[0].date_time_);
    localtime = start_time;
  }

  // Expand using adjacency list until we exceed threshold
  uint32_t n = 0;
  bool date_set = false;
  uint32_t blockid, tripid;
  std::unordered_map<std::string, uint32_t> operators;
  std::unordered_set<uint32_t> processed_tiles;
  const GraphTile* tile;
  while (true) {
    // Get next element from adjacency list. Check that it is valid. An
    // invalid label indicates there are no edges that can be expanded.
    uint32_t predindex = adjacencylist_->pop();
    if (predindex == kInvalidLabel) {
      return isotile_;
    }

    // Copy the EdgeLabel for use in costing and settle the edge.
    EdgeLabel pred = edgelabels_[predindex];
    edgestatus_->Update(pred.edgeid(), EdgeSet::kPermanent);

    // Get the end node. Skip if tile not found (can happen with
    // regional data sets).
    GraphId node = pred.endnode();
    if ((tile = graphreader.GetGraphTile(node)) == nullptr) {
      continue;
    }

    // Get the nodeinfo and update the isotile
    const NodeInfo* nodeinfo = tile->node(node);
    UpdateIsoTile(pred, graphreader, nodeinfo->latlng());
    n++;

    // Return after the time interval has been met
    if (pred.cost().secs > max_seconds) {
      LOG_DEBUG("Exceed time interval: n = " + std::to_string(n));
      return isotile_;
    }

    // Check access at the node
    if (!costing->Allowed(nodeinfo)) {
      continue;
    }

    // Set local time. TODO: adjust for time zone.
    uint32_t localtime = start_time + pred.cost().secs;

    // Set a default transfer penalty at a stop (if not same trip Id and block Id)
    Cost transfer_cost = tc->DefaultTransferCost();

    // Get any transfer times and penalties if this is a transit stop (and
    // transit has been taken at some point on the path) and mode is pedestrian
    mode_ = pred.mode();
    bool has_transit = pred.has_transit();
    GraphId prior_stop = pred.prior_stopid();
    uint32_t operator_id = pred.transit_operator();
    if (nodeinfo->type() == NodeType::kMultiUseTransitStop) {
      // Get the transfer penalty when changing stations
      if (mode_ == TravelMode::kPedestrian && prior_stop.Is_Valid() && has_transit) {
        transfer_cost = tc->TransferCost();
      }

      if (processed_tiles.find(tile->id().tileid()) == processed_tiles.end()) {
        tc->AddToExcludeList(tile);
        processed_tiles.emplace(tile->id().tileid());
      }

      //check if excluded.
      if (tc->IsExcluded(tile, nodeinfo))
        continue;

      // Add transfer time to the local time when entering a stop
      // as a pedestrian. This is a small added cost on top of
      // any costs along paths and roads
      if (mode_ == TravelMode::kPedestrian) {
        localtime += transfer_cost.secs;
      }

      // Update prior stop. TODO - parent/child stop info?
      prior_stop = node;

      // we must get the date from level 3 transit tiles and not level 2.  The level 3 date is
      // set when the fetcher grabbed the transit data and created the schedules.
      if (!date_set) {
        date = DateTime::days_from_pivot_date(DateTime::get_formatted_date(*origin_locations[0].date_time_));
        dow  = DateTime::day_of_week_mask(*origin_locations[0].date_time_);
        uint32_t date_created = tile->header()->date_created();
        if (date < date_created)
          date_before_tile = true;
        else
          day = date - date_created;

        date_set = true;
      }
    }

    // TODO: allow mode changes at special nodes
    //      bike share (pedestrian <--> bicycle)
    //      parking (drive <--> pedestrian)
    //      transit stop (pedestrian <--> transit).
    bool mode_change = false;

    // Expand from end node.
    uint32_t shortcuts = 0;
    GraphId edgeid(node.tileid(), node.level(), nodeinfo->edge_index());
    const DirectedEdge* directededge = tile->directededge(nodeinfo->edge_index());
    for (uint32_t i = 0; i < nodeinfo->edge_count();
                i++, directededge++, edgeid++) {
      // Skip shortcut edges
      if (directededge->is_shortcut()) {
        continue;
      }

      // Get the current set. Skip this edge if permanently labeled (best
      // path already found to this directed edge).
      EdgeStatusInfo edgestatus = edgestatus_->Get(edgeid);
      if (edgestatus.set() == EdgeSet::kPermanent) {
        continue;
      }

      // Handle transition edges. Add to adjacency list using predecessor
      // information.
      if (directededge->trans_up() || directededge->trans_down()) {
        uint32_t idx = edgelabels_.size();
        adjacencylist_->add(idx, pred.sortcost());
        edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
        edgelabels_.emplace_back(predindex, edgeid, directededge->endnode(), pred);
        continue;
      }

      // Reset cost and walking distance
      Cost newcost = pred.cost();
      uint32_t walking_distance = pred.path_distance();
      // If this is a transit edge - get the next departure. Do not check
      // if allowed by costing - assume if you get a transit edge you
      // walked to the transit stop
      tripid  = 0;
      blockid = 0;
      if (directededge->IsTransitLine()) {
        // Check if transit costing allows this edge
        if (!tc->Allowed(directededge, pred, tile, edgeid)) {
          continue;
        }

        //check if excluded.
        if (tc->IsExcluded(tile, directededge))
          continue;

        // Look up the next departure along this edge
        const TransitDeparture* departure = tile->GetNextDeparture(
                    directededge->lineid(), localtime, day, dow, date_before_tile,
                    wheelchair, bicycle);
        if (departure) {
          // Check if there has been a mode change
          mode_change = (mode_ == TravelMode::kPedestrian);

          // Update trip Id and block Id
          tripid  = departure->tripid();
          blockid = departure->blockid();
          has_transit = true;

          // There is no cost to remain on the same trip or valid blockId
          if ( tripid == pred.tripid() ||
              (blockid != 0 && blockid == pred.blockid())) {
            // This departure is valid without any added cost. Operator Id
            // is the same as the predecessor
            operator_id = pred.transit_operator();
          } else {
            if (pred.tripid() > 0) {
              // tripId > 0 means the prior edge was a transit edge and this
              // is an "in-station" transfer. Add a small transfer time and
              // call GetNextDeparture again if we cannot make the current
              // departure.
              // TODO - is there a better way?
              if (localtime + 30 > departure->departure_time()) {
                  departure = tile->GetNextDeparture(directededge->lineid(),
                                localtime + 30, day, dow, date_before_tile,
                                wheelchair, bicycle);
                if (!departure)
                  continue;
              }
            }

            // Get the operator Id
            operator_id = GetOperatorId(tile, departure->routeid(), operators);

            // Add transfer penalty and operator change penalty
            newcost.cost += transfer_cost.cost;
            if (pred.transit_operator() > 0 &&
                pred.transit_operator() != operator_id) {
              // TODO - create a configurable operator change penalty
              newcost.cost += 300;
            }
          }

          // Change mode and costing to transit. Add edge cost.
          mode_ = TravelMode::kPublicTransit;
          newcost += tc->EdgeCost(directededge, departure, localtime);
        } else {
          // No matching departures found for this edge
          continue;
        }
      } else {
        // If current mode is public transit we should only connect to
        // transit connection edges or transit edges
        if (mode_ == TravelMode::kPublicTransit) {
          // Disembark from transit and reset walking distance
          mode_ = TravelMode::kPedestrian;
          walking_distance = 0;
          mode_change = true;
        }

        // Regular edge - use the appropriate costing and check if access
        // is allowed. If mode is pedestrian this will validate walking
        // distance has not been exceeded.
        if (!mode_costing[static_cast<uint32_t>(mode_)]->Allowed(
                directededge, pred, tile, edgeid)) {
          continue;
        }

        Cost c = mode_costing[static_cast<uint32_t>(mode_)]->EdgeCost(directededge);
        c.cost *= mode_costing[static_cast<uint32_t>(mode_)]->GetModeWeight();
        newcost += c;

        // Add to walking distance
        if (mode_ == TravelMode::kPedestrian) {
          walking_distance += directededge->length();

          // Prevent going from one transit connection directly to another
          // at a transit stop - this is like entering a station and exiting
          // without getting on transit
          if (nodeinfo->type() == NodeType::kMultiUseTransitStop &&
              pred.use()   == Use::kTransitConnection &&
              directededge->use()  == Use::kTransitConnection)
                continue;
        }
      }

      // Add mode change cost or edge transition cost from the costing model
      if (mode_change) {
        // TODO: make mode change cost configurable. No cost for entering
        // a transit line (assume the wait time is the cost)
        ;  //newcost += {10.0f, 10.0f };
      } else {
        newcost += mode_costing[static_cast<uint32_t>(mode_)]->TransitionCost(
               directededge, nodeinfo, pred);
      }

      // Prohibit entering the same station as the prior.
      if (directededge->use() == Use::kTransitConnection &&
          directededge->endnode() == pred.prior_stopid()) {
        continue;
      }

      // Test if exceeding maximum transfer walking distance
      if (directededge->use() == Use::kTransitConnection &&
          pred.prior_stopid().Is_Valid() &&
          walking_distance > max_transfer_distance) {
        continue;
      }

      // Continue if the time interval has been met...
      // this bus or rail line goes beyond the max but need to consider others
      // so we just continue here.
      if (newcost.secs > max_seconds) {
        continue;
      }

      // Check if edge is temporarily labeled and this path has less cost. If
      // less cost the predecessor is updated and the sort cost is decremented
      // by the difference in real cost (A* heuristic doesn't change). Update
      // trip Id and block Id.
      if (edgestatus.set() == EdgeSet::kTemporary) {
        uint32_t idx = edgestatus.index();
        float dc = edgelabels_[idx].cost().cost - newcost.cost;
        if (dc > 0) {
          float oldsortcost = edgelabels_[idx].sortcost();
          float newsortcost = oldsortcost - dc;
          edgelabels_[idx].Update(predindex, newcost, newsortcost,
                                  walking_distance, tripid, blockid);
          adjacencylist_->decrease(idx, newsortcost, oldsortcost);
        }
        continue;
      }

      // Add edge label, add to the adjacency list and set edge status
      uint32_t idx = edgelabels_.size();
      adjacencylist_->add(idx, newcost.cost);
      edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
      edgelabels_.emplace_back(predindex, edgeid, directededge,
                    newcost, newcost.cost, 0.0f, mode_, walking_distance,
                    tripid, prior_stop, blockid, operator_id, has_transit);
    }
  }
  return isotile_;      // Should never get here
}

// Update the isotile
void Isochrone::UpdateIsoTile(const EdgeLabel& pred, GraphReader& graphreader,
                              const PointLL& ll) {
  // Skip if the opposing edge has already been settled.
  GraphId opp = graphreader.GetOpposingEdgeId(pred.edgeid());
  EdgeStatusInfo edgestatus = edgestatus_->Get(opp);
  if (edgestatus.set() == EdgeSet::kPermanent) {
      return;
  }

  // Get the DirectedEdge because we'll need its shape
  const GraphTile* tile = graphreader.GetGraphTile(pred.edgeid().Tile_Base());
  const DirectedEdge* edge = tile->directededge(pred.edgeid());
  // Transit lines can't really be "reached" you really just pass through those cells
  if(edge->IsTransitLine())
    return;

  // Get time at the end node of the predecessor
  float secs1 = pred.cost().secs;

  // Get the time at the end node of the predecessor
  float secs0;
  uint32_t predindex = pred.predecessor();
  if (predindex == kInvalidLabel) {
    //TODO - do we need partial shape from origin location to end of edge?
    secs0 = 0;
  } else {
    secs0 = edgelabels_[predindex].cost().secs;
  }

  // Get the shape and make sure shape is forward
  // direction and resample it to the shape interval.
  auto shape = tile->edgeinfo(edge->edgeinfo_offset()).shape();
  if (!edge->forward()) {
    std::reverse(shape.begin(), shape.end());
  }
  auto resampled = resample_spherical_polyline(shape, shape_interval_);

  // Mark grid cells along the shape if time is less than what is
  // already populated. Get intersection of tiles along each segment
  // so this doesn't miss shape that crosses tile corners
  float delta = (shape_interval_ * (secs1 - secs0)) / edge->length();
  float secs = secs0;
  auto itr1 = resampled.begin();
  auto itr2 = itr1 + 1;
  for (auto itr2 = itr1 + 1; itr2 < resampled.end(); itr1++, itr2++) {
    secs += delta;
    auto tiles = isotile_->Intersect(std::list<PointLL>{*itr1, *itr2});
    for (auto t : tiles) {
      isotile_->SetIfLessThan(t.first, secs * to_minutes);
    }
  }
}

// Check if edge is temporarily labeled and this path has less cost. If
// less cost the predecessor is updated and the sort cost is decremented
// by the difference in real cost (A* heuristic doesn't change)
void Isochrone::CheckIfLowerCostPath(const uint32_t idx,
                                     const uint32_t predindex,
                                     const Cost& newcost) {
  float dc = edgelabels_[idx].cost().cost - newcost.cost;
  if (dc > 0) {
    float oldsortcost = edgelabels_[idx].sortcost();
    float newsortcost = oldsortcost - dc;
    edgelabels_[idx].Update(predindex, newcost, newsortcost);
    adjacencylist_->decrease(idx, newsortcost, oldsortcost);
  }
}

// Add edge(s) at each origin to the adjacency list
void Isochrone::SetOriginLocations(GraphReader& graphreader,
                 std::vector<PathLocation>& origin_locations,
                 const std::shared_ptr<DynamicCost>& costing) {
  // Add edges for each location to the adjacency list
  for (auto& origin : origin_locations) {
    // Set time at the origin lat, lon grid to 0
    isotile_->Set(origin.latlng_, 0);

    // Iterate through edges and add to adjacency list
    const NodeInfo* nodeinfo = nullptr;
    for (const auto& edge : (origin.edges)) {
      // If origin is at a node - skip any inbound edge (dist = 1)
      if (edge.end_node()) {
        continue;
      }

      // Get the directed edge
      GraphId edgeid = edge.id;
      const GraphTile* tile = graphreader.GetGraphTile(edgeid);
      const DirectedEdge* directededge = tile->directededge(edgeid);

      // Set the tile creation date
      tile_creation_date_ = tile->header()->date_created();

      // Get the tile at the end node. Skip if tile not found as we won't be
      // able to expand from this origin edge.
      const GraphTile* endtile = graphreader.GetGraphTile(directededge->endnode());
      if (endtile == nullptr) {
        continue;
      }

      // Get cost
      nodeinfo = endtile->node(directededge->endnode());
      Cost cost = costing->EdgeCost(directededge) * (1.0f - edge.dist);

      // Add EdgeLabel to the adjacency list (but do not set its status).
      // Set the predecessor edge index to invalid to indicate the origin
      // of the path.
      uint32_t idx = edgelabels_.size();
      uint32_t d = static_cast<uint32_t>(directededge->length() * (1.0f - edge.dist));
      adjacencylist_->add(idx, cost.cost);
      edgestatus_->Set(edgeid, EdgeSet::kTemporary, idx);
      EdgeLabel edge_label(kInvalidLabel, edgeid, directededge, cost,
                           cost.cost, 0.0f, mode_, d);
      edge_label.set_origin();

      // Set the origin flag
      edgelabels_.push_back(std::move(edge_label));
    }

    // Set the origin timezone
    if (nodeinfo != nullptr && origin.date_time_ &&
      *origin.date_time_ == "current") {
      origin.date_time_ = DateTime::iso_date_time(
          DateTime::get_tz_db().from_index(nodeinfo->timezone()));
    }
  }
}

// Add destination edges to the reverse path adjacency list.
void Isochrone::SetDestinationLocations(GraphReader& graphreader,
                     std::vector<PathLocation>& dest_locations,
                     const std::shared_ptr<DynamicCost>& costing) {
  // Add edges for each location to the adjacency list
  for (auto& dest : dest_locations) {
    // Set time at the origin lat, lon grid to 0
    isotile_->Set(dest.latlng_, 0);

    // Iterate through edges and add to adjacency list
    Cost c;
    for (const auto& edge : dest.edges) {
      // If the destination is at a node, skip any outbound edges (so any
      // opposing inbound edges are not considered)
      if (edge.begin_node()) {
        continue;
      }

      // Get the directed edge
      GraphId edgeid = edge.id;
      const GraphTile* tile = graphreader.GetGraphTile(edgeid);
      const DirectedEdge* directededge = tile->directededge(edgeid);

      // Get the opposing directed edge, continue if we cannot get it
      GraphId opp_edge_id = graphreader.GetOpposingEdgeId(edgeid);
      if (!opp_edge_id.Is_Valid()) {
        continue;
      }
      const DirectedEdge* opp_dir_edge = graphreader.GetOpposingEdge(edgeid);

      // Get cost and sort cost (based on distance from endnode of this edge
      // to the origin. Make sure we use the reverse A* heuristic. Note that
      // the end node of the opposing edge is in the same tile as the directed
      // edge.  Use the directed edge for costing, as this is the forward
      // direction along the destination edge.
      Cost cost = costing->EdgeCost(directededge) * edge.dist;

      // Add EdgeLabel to the adjacency list. Set the predecessor edge index
      // to invalid to indicate the origin of the path. Make sure the opposing
      // edge (edgeid) is set.
      uint32_t idx = edgelabels_.size();
      adjacencylist_->add(idx, cost.cost);
      edgestatus_->Set(opp_edge_id, EdgeSet::kTemporary, idx);
      edgelabels_.emplace_back(kInvalidLabel, opp_edge_id, edgeid,
                  opp_dir_edge, cost, cost.cost, 0.0f, mode_, c, false);
    }
  }
}

}
}
