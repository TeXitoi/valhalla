#include "loki/search.h"
#include "midgard/linesegment2.h"
#include "midgard/distanceapproximator.h"

#include <unordered_set>
#include <list>
#include <math.h>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;
using namespace valhalla::loki;

namespace {
//the cutoff at which we will assume the input is too far away from civilisation to be
//worth correlating to the nearest graph elements
constexpr float SEARCH_CUTOFF = 35000.f;
//during edge correlation, if you end up < 5 meters from the beginning or end of the
//edge we just assume you were at that node and not actually along the edge
//we keep it small because point and click interfaces are more accurate than gps input
constexpr float NODE_SNAP = 5.f;
//during side of street computations we figured you're on the street if you are less than
//5 meters (16) feet from the centerline. this is actually pretty large (with accurate shape
//data for the roads it might want half that) but its better to assume on street than not
constexpr float SIDE_OF_STREET_SNAP = 5.f;
//if you are this far away from the edge we are considering and you set a heading we will
//ignore it because its not really useful at this distance from the geometry
constexpr float NO_HEADING = 30.f;
//how much of the shape should be sampled to get heading
constexpr float HEADING_SAMPLE = 30.f;
//cone width to use for cosine similarity comparisons for favoring heading
constexpr float DEFAULT_ANGLE_WIDTH = 60.f;

std::function<std::tuple<int32_t, unsigned short, float>()>
make_binner(const PointLL& p, const GraphReader& reader) {
  const auto& tiles = reader.GetTileHierarchy().levels().rbegin()->second.tiles;
  return tiles.ClosestFirst(p);
}

// Model a segment (2 consecutive points in an edge in a bin).
struct candidate_t {
  float sq_distance;
  PointLL point;
  size_t index;

  GraphId edge_id;
  const DirectedEdge* edge;
  std::shared_ptr<const EdgeInfo> edge_info;

  const GraphTile* tile;
};

// This structure contains the context of the projection of a
// Location.  At the creation, a bin is affected to the point.  The
// test() method should be called to each valid segment of the bin.
// When the bin is finished, next_bin() switch to the next possible
// interesting bin.  if has_bin() is false, then the best projection
// is found.
struct projector_t {
  projector_t(const Location& location, GraphReader& reader):
    binner(make_binner(location.latlng_, reader)),
    location(location), candidate{std::numeric_limits<float>::max()},
    lon_scale(cosf(location.latlng_.lat() * kRadPerDeg)),
    lat(location.latlng_.lat()),
    lng(location.latlng_.lng()),
    approx(location.latlng_) {
    next_bin(reader);
  }

  // non default constructible and move only type
  projector_t() = delete;
  projector_t(const projector_t&) = delete;
  projector_t& operator=(const projector_t&) = delete;
  projector_t(projector_t&&) = default;
  projector_t& operator=(projector_t&&) = default;

  // basic getters
  const PointLL& point() const { return location.latlng_; }
  bool has_bin() const { return cur_tile != nullptr; }
  bool projection_found() const { return candidate.point.IsValid(); }

  // Sort the candidate_t by bin so that the ones we still need to do are at the front
  // this way we can keep searching until the first one is finished
  bool operator<(const projector_t& other) const {
    if (cur_tile != other.cur_tile)
      return cur_tile > other.cur_tile;// nullptr at the end
    return bin_index < other.bin_index;
  }

  bool has_same_bin(const projector_t& other) const {
    return cur_tile == other.cur_tile && bin_index == other.bin_index;
  }

  // Advance to the next bin. Must not be called if has_bin() is false.
  void next_bin(GraphReader& reader) {
    do {
      //TODO: make configurable the radius at which we give up searching
      //the closest thing in this bin is further than what we have already
      auto bin = binner();
      if(std::get<2>(bin) > SEARCH_CUTOFF || std::get<2>(bin) > sqrt(candidate.sq_distance)) {
        cur_tile = nullptr;
        break;
      }

      //grab the tile the lat, lon is in
      auto tile_id = GraphId(std::get<0>(bin), reader.GetTileHierarchy().levels().rbegin()->first, 0);
      reader.GetGraphTile(tile_id, cur_tile);
      bin_index = std::get<1>(bin);
    } while (! cur_tile);
  }

  // Test if a segment is a candidate to the projection.  This method
  // is performance critical.  Copy, function call, cache locality and
  // useless computation must be handled with care.
  PointLL project(const PointLL& u, const PointLL& v) {
    //project a onto b where b is the origin vector representing this segment
    //and a is the origin vector to the point we are projecting, (a.b/b.b)*b
    auto bx = v.first - u.first;
    auto by = v.second - u.second;

    // Scale longitude when finding the projection. Avoid divided-by-zero
    // which gives a NaN scale, otherwise comparisons below will fail
    auto bx2 = bx * lon_scale;
    auto sq = bx2*bx2 + by*by;
    auto scale = sq > 0 ? ((lng - u.lng())*lon_scale*bx2 + (lat - u.lat())*by) / sq : 0.f;

    //projects along the ray before u
    if(scale <= 0.f)
      return u;
    //projects along the ray after v
    else if(scale >= 1.f)
      return v;
    //projects along the ray between u and v
    return { u.first+bx*scale, u.second+by*scale };
  }

  std::function<std::tuple<int32_t, unsigned short, float>()> binner;
  const GraphTile* cur_tile = nullptr;
  Location location;
  unsigned short bin_index = 0;
  candidate_t candidate;

  // critical data
  float lon_scale;
  float lat;
  float lng;
  DistanceApproximator approx;
};

//TODO: move this to midgard and test the crap out of it
//we are essentially estimating the angle of the tangent
//at a point along a discretised curve. we attempt to mostly
//use the shape coming into the point on the curve but if there
//isnt enough there we will use the shape coming out of the it
float tangent_angle(size_t index, const PointLL& point, const std::vector<PointLL>& shape, bool forward) {
  //depending on if we are going forward or backward we choose a different increment
  auto increment = forward ? -1 : 1;
  auto first_end = forward ? shape.cbegin() : shape.cend() - 1 ;
  auto second_end = forward ? shape.cend() - 1 : shape.cbegin();

  //u and v will be points we move along the shape until we have enough distance between them or run out of points

  //move backwards until we have enough or run out
  float remaining = HEADING_SAMPLE;
  auto u = point;
  auto i = shape.cbegin() + index + forward;
  while(remaining > 0 && i != first_end) {
    //move along and see how much distance that added
    i += increment;
    auto d = u.Distance(*i);
    //are we done yet?
    if(remaining <= d) {
      auto coef = remaining / d;
      u = u.AffineCombination(1 - coef, coef, *i);
      return u.Heading(point);
    }
    //next one
    u = *i;
    remaining -= d;
  }

  //move forwards until we have enough or run out
  auto v = point;
  i = shape.cbegin() + index + !forward;
  while(remaining > 0 && i != second_end) {
    //move along and see how much distance that added
    i -= increment;
    auto d = v.Distance(*i);
    //are we done yet?
    if(remaining <= d) {
      auto coef = remaining / d;
      v = v.AffineCombination(1 - coef, coef, *i);
      return u.Heading(v);
    }
    //next one
    v = *i;
    remaining -= d;
  }

  return u.Heading(v);
}

bool heading_filter(const DirectedEdge* edge, const EdgeInfo& info,
  const Location& location, const PointLL& point, float distance, size_t index) {
  //if its far enough away from the edge, the heading is pretty useless
  if(!location.heading_ || distance > NO_HEADING)
    return false;

  //get the angle of the shape from this point
  auto angle = tangent_angle(index, point, info.shape(), edge->forward());
  //we want the closest distance between two angles which can be had
  //across 0 or between the two so we just need to know which is bigger
  if(*location.heading_ > angle)
    return std::min(*location.heading_ - angle, (360.f - *location.heading_) + angle) > location.heading_tolerance_.get_value_or(DEFAULT_ANGLE_WIDTH);
  return std::min(angle - *location.heading_, (360.f - angle) + *location.heading_) > location.heading_tolerance_.get_value_or(DEFAULT_ANGLE_WIDTH);
}

PathLocation::SideOfStreet flip_side(const PathLocation::SideOfStreet side) {
  if(side != PathLocation::SideOfStreet::NONE)
    return side == PathLocation::SideOfStreet::LEFT ? PathLocation::SideOfStreet::RIGHT : PathLocation::SideOfStreet::LEFT;
  return side;
}

PathLocation::SideOfStreet get_side(const candidate_t& candidate, const PointLL& original, float distance){
  //its so close to the edge that its basically on the edge
  if(distance < SIDE_OF_STREET_SNAP)
    return PathLocation::SideOfStreet::NONE;

  //if the projected point is way too close to the begin or end of the shape
  //TODO: if the original point is really far away side of street may also not make much sense..
  auto& shape = candidate.edge_info->shape();
  if(candidate.point.Distance(shape.front()) < SIDE_OF_STREET_SNAP ||
     candidate.point.Distance(shape.back())  < SIDE_OF_STREET_SNAP)
    return PathLocation::SideOfStreet::NONE;

  //get the side TODO: this can technically fail for longer segments..
  //to fix it we simply compute the plane formed by the triangle
  //through the center of the earth and the two shape points and test
  //whether the original point is above or below the plane (depending on winding)
  LineSegment2<PointLL> segment(shape[candidate.index], shape[candidate.index + 1]);
  return (segment.IsLeft(original) > 0) == candidate.edge->forward()  ? PathLocation::SideOfStreet::LEFT : PathLocation::SideOfStreet::RIGHT;
}

PathLocation correlate_node(GraphReader& reader, const Location& location, const EdgeFilter& edge_filter, const GraphId& found_node, const candidate_t& candidate){
  PathLocation correlated(location);
  auto distance = location.latlng_.Distance(candidate.point);
  std::list<PathLocation::PathEdge> heading_filtered;

  //we need this because we might need to go to different levels
  std::function<void (const GraphId& node_id, bool transition)> crawl;
  crawl = [&](const GraphId& node_id, bool follow_transitions) {
    //now that we have a node we can pass back all the edges leaving and entering it
    const auto* tile = reader.GetGraphTile(node_id);
    if(!tile)
      return;
    const auto* node = tile->node(node_id);
    const auto* start_edge = tile->directededge(node->edge_index());
    const auto* end_edge = start_edge + node->edge_count();
    for(const auto* edge = start_edge; edge < end_edge; ++edge) {
      //if this is an edge leaving this level then we should go do that level awhile
      if(follow_transitions && (edge->trans_down() || edge->trans_up()))
        crawl(edge->endnode(), false);

      //get some info about this edge and the opposing
      GraphId id = tile->id();
      id.fields.id = node->edge_index() + (edge - start_edge);
      auto info = tile->edgeinfo(edge->edgeinfo_offset());

      //do we want this edge
      if(edge_filter(edge) != 0.0f) {
        PathLocation::PathEdge path_edge{std::move(id), 0.f, node->latlng(), distance, PathLocation::NONE};
        auto index = edge->forward() ? 0 : info.shape().size() - 2;
        if(!heading_filter(edge, info, location, candidate.point, distance, index))
          correlated.edges.push_back(std::move(path_edge));
        else
          heading_filtered.emplace_back(std::move(path_edge));
      }

      //do we want the evil twin
      const GraphTile* other_tile;
      const auto other_id = reader.GetOpposingEdgeId(id, other_tile);
      if(!other_tile)
        continue;
      const auto* other_edge = other_tile->directededge(other_id);
      if(edge_filter(other_edge) != 0.0f) {
        PathLocation::PathEdge path_edge{std::move(other_id), 1.f, node->latlng(), distance, PathLocation::NONE};
        auto index = other_edge->forward() ? 0 : info.shape().size() - 2;
        if(!heading_filter(other_edge, tile->edgeinfo(edge->edgeinfo_offset()), location, candidate.point, distance, index))
          correlated.edges.push_back(std::move(path_edge));
        else
          heading_filtered.emplace_back(std::move(path_edge));
      }
    }

    //if we have nothing because of heading we'll just ignore it
    if(correlated.edges.size() == 0 && heading_filtered.size())
      for(auto& path_edge : heading_filtered)
        correlated.edges.push_back(std::move(path_edge));

    //if we still found nothing that is no good..
    if(correlated.edges.size() == 0)
      throw std::runtime_error("No suitable edges near location");
  };

  //start where we are and crawl from there
  crawl(found_node, true);

  //if it was a through location with a heading its pretty confusing.
  //does the user want to come into and exit the location at the preferred
  //angle? for now we are just saying that they want it to exit at the
  //heading provided. this means that if it was node snapped we only
  //want the outbound edges
  if(location.stoptype_ == Location::StopType::THROUGH && location.heading_) {
    auto new_end = std::remove_if(correlated.edges.begin(), correlated.edges.end(),
      [](const PathLocation::PathEdge& e) { return e.end_node(); });
    correlated.edges.erase(new_end, correlated.edges.end());
  }

  if(correlated.edges.size() == 0)
    throw std::runtime_error("No suitable edges near location");

  //give it back
  return correlated;
}

PathLocation correlate_edge(GraphReader& reader, const Location& location, const EdgeFilter& edge_filter, const candidate_t& candidate) {
  //now that we have an edge we can pass back all the info about it
  PathLocation correlated(location);
  auto distance = location.latlng_.Distance(candidate.point);
  if(candidate.edge != nullptr){
    //we need the ratio in the direction of the edge we are correlated to
    double partial_length = 0;
    for(size_t i = 0; i < candidate.index; ++i)
      partial_length += candidate.edge_info->shape()[i].Distance(candidate.edge_info->shape()[i + 1]);
    partial_length += candidate.edge_info->shape()[candidate.index].Distance(candidate.point);
    partial_length = std::min(partial_length, static_cast<double>(candidate.edge->length()));
    float length_ratio = static_cast<float>(partial_length / static_cast<double>(candidate.edge->length()));
    if(!candidate.edge->forward())
      length_ratio = 1.f - length_ratio;
    //side of street
    auto side = get_side(candidate, location.latlng_, distance);
    //correlate the edge we found
    std::list<PathLocation::PathEdge> heading_filtered;
    if(heading_filter(candidate.edge, *candidate.edge_info, location, candidate.point, distance, candidate.index))
      heading_filtered.emplace_back(candidate.edge_id, length_ratio, candidate.point, side);
    else
      correlated.edges.push_back(PathLocation::PathEdge{candidate.edge_id, length_ratio, candidate.point, distance, side});
    //correlate its evil twin
    const GraphTile* other_tile;
    auto opposing_edge_id = reader.GetOpposingEdgeId(candidate.edge_id, other_tile);
    const DirectedEdge* other_edge;
    if(opposing_edge_id.Is_Valid() && (other_edge = other_tile->directededge(opposing_edge_id)) && edge_filter(other_edge) != 0.0f) {
      if(heading_filter(other_edge, *candidate.edge_info, location, candidate.point, distance, candidate.index))
        heading_filtered.emplace_back(opposing_edge_id, 1 - length_ratio, candidate.point, distance, flip_side(side));
      else
        correlated.edges.push_back(PathLocation::PathEdge{opposing_edge_id, 1 - length_ratio, candidate.point, distance, flip_side(side)});
    }

    //if we have nothing because of heading we'll just ignore it
    if(correlated.edges.size() == 0 && heading_filtered.size())
      for(auto& path_edge : heading_filtered)
        correlated.edges.push_back(std::move(path_edge));
  }

  //if we found nothing that is no good..
  if(correlated.edges.size() == 0)
    throw std::runtime_error("No suitable edges near location");

  //give it back
  return correlated;
}

// Test if this location is an isolated "island" without connectivity to the
// larger routing graph. Does a breadth first search - if possible paths are
// exhausted within some threshold this returns a set of edges within the
// island.
std::unordered_set<GraphId> island(const PathLocation& location,
             GraphReader& reader, const NodeFilter& node_filter,
             const EdgeFilter& edge_filter, const uint32_t edge_threshold,
             const uint32_t length_threshold, const uint32_t node_threshold) {
  std::unordered_set<GraphId> todo(edge_threshold);
  std::unordered_set<GraphId> done(edge_threshold);

  // Seed the list of edges to expand
  for (const auto& edge : location.edges) {
    todo.insert(edge.id);
  }

  // We are done if we hit a threshold meaning it isn't an island or we ran
  // out of edges and we determine it is an island
  uint32_t total_edge_length = 0;
  uint32_t nodes_expanded = 0;
  while ((done.size() < edge_threshold || total_edge_length < length_threshold ||
          nodes_expanded < node_threshold) && todo.size()) {
    // Get the next edge
    const GraphId edge = *todo.cbegin();
    done.emplace(edge);

    // Get the directed edge - filter it out if not accessible
    const DirectedEdge* directededge = reader.GetGraphTile(edge)->directededge(edge);
    if (edge_filter(directededge) == 0.0f) {
      continue;
    }
    total_edge_length += directededge->length();

    // Get the end node - filter it out if not accessible
    const GraphId node = directededge->endnode();
    const GraphTile* tile = reader.GetGraphTile(node);
    const NodeInfo* nodeinfo = tile->node(node);
    if (node_filter(nodeinfo)) {
      continue;
    }

    // Expand edges from the node
    bool expanded = false;
    GraphId edgeid(node.tileid(), node.level(), nodeinfo->edge_index());
    directededge = tile->directededge(nodeinfo->edge_index());
    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, edgeid++) {
      // Skip transition edges, transit connection edges, and edges that are not allowed
      if (directededge->trans_up() || directededge->trans_down() ||
          directededge->use() == Use::kTransitConnection ||
          edge_filter(directededge) == 0.0f) {
        continue;
      }

      // Add to the todo list
      todo.emplace(edgeid);
      expanded = true;
    }
    nodes_expanded += expanded;
  }

  // If there are still edges to do then we broke out of the loop above due to
  // meeting thresholds and this is not a disconnected island. If there are no
  // more edges then this is a disconnected island and we want to know what
  // edges constitute the island so a second pass can avoid them
  return (todo.size() == 0) ? done : std::unordered_set<GraphId>{};
}

// Handle a bin for a range of candidate_t.  Every candidate_t in
// the range must be on the same bin.  The bin will be read, the
// segment passed to the candidate_ts and the candidate_ts will
// advance their bins.
void handle_bin(std::vector<projector_t>::iterator pp_begin,
                std::vector<projector_t>::iterator pp_end,
                GraphReader& reader,
                const EdgeFilter& edge_filter) {
  //iterate over the edges in the bin
  auto tile = pp_begin->cur_tile;
  auto edges = tile->GetBin(pp_begin->bin_index);
  for(auto e : edges) {
    //get the tile and edge
    if(!reader.GetGraphTile(e, tile))
      continue;

    //no thanks on this one or its evil twin
    const auto* edge = tile->directededge(e);
    if(edge_filter(edge) == 0.0f &&
        (!(e = reader.GetOpposingEdgeId(e, tile)).Is_Valid() ||
         edge_filter(edge = tile->directededge(e)) == 0.0f)) {
      continue;
    }

    //get some shape of the edge
    auto edge_info = std::make_shared<const EdgeInfo>(tile->edgeinfo(edge->edgeinfo_offset()));
    auto shape = edge_info->lazy_shape();
    PointLL v;
    if (!shape.empty())
      v = shape.pop();
    
    //iterate along this edges segments projecting each of the points
    for(size_t i = 0; !shape.empty(); ++i) {
      const auto u = v;
      v = shape.pop();
      for (auto it = pp_begin; it != pp_end; ++it) {
        auto point = it->project(u, v);
        const auto sq_distance = it->approx.DistanceSquared(point);
        // this block is not in the hot spot
        if(sq_distance < it->candidate.sq_distance) {
          it->candidate = candidate_t{sq_distance, std::move(point), 0, e, edge, edge_info, tile};
        }
      }
    }
  }

  // bin is finished, advance the candidate_ts to their respective
  // next bin.
  for (auto it = pp_begin; it != pp_end; ++it) {
    it->next_bin(reader);
  }
}

// Create the PathLocation corresponding to the best projection of the
// given candidate_t.
PathLocation finalize(const projector_t& pp, GraphReader& reader, const EdgeFilter& edge_filter) {
  //keep track of bins we looked in but only the ones that had something
  //would rather log this in the service only, so lets figure a way to pass it back
  //midgard::logging::Log("valhalla_loki_bins_searched::" + std::to_string(bins), " [ANALYTICS] ");

  //this may be at a node, either because it was the closest thing or from snap tolerance
  bool front = pp.candidate.point == pp.candidate.edge_info->shape().front() ||
    pp.point().Distance(pp.candidate.edge_info->shape().front()) < NODE_SNAP;
  bool back = pp.candidate.point == pp.candidate.edge_info->shape().back() ||
    pp.point().Distance(pp.candidate.edge_info->shape().back()) < NODE_SNAP;
  //it was the begin node
  if((front && pp.candidate.edge->forward()) || (back && !pp.candidate.edge->forward())) {
    const GraphTile* other_tile;
    auto opposing_edge = reader.GetOpposingEdge(pp.candidate.edge_id, other_tile);
    if(!other_tile)
      throw std::runtime_error("No suitable edges near location");
    return correlate_node(reader, pp.location, edge_filter, opposing_edge->endnode(), pp.candidate);
  }
  //it was the end node
  if((back && pp.candidate.edge->forward()) || (front && !pp.candidate.edge->forward()))
    return correlate_node(reader, pp.location, edge_filter, pp.candidate.edge->endnode(), pp.candidate);

  //it was along the edge
  return correlate_edge(reader, pp.location, edge_filter, pp.candidate);
}

// Find the best range to do.  The given vector should be sorted for
// interesting grouping.  Returns the greatest range of non empty
// equal bins.
std::pair<std::vector<projector_t>::iterator, std::vector<projector_t>::iterator>
find_best_range(std::vector<projector_t>& pps) {
  auto best = std::make_pair(pps.begin(), pps.begin());
  auto cur = best;
  while (cur.second != pps.end()) {
    cur.first = cur.second;
    cur.second = std::find_if_not(cur.first, pps.end(), [&cur](const projector_t& pp) {
      return cur.first->has_same_bin(pp);
    });
    if (cur.first->has_bin() && cur.second - cur.first > best.second - best.first) {
      best = cur;
    }
  }
  return best;
}

}

namespace valhalla {
namespace loki {

std::unordered_map<Location, PathLocation>
Search(const std::vector<Location>& locations, GraphReader& reader, const EdgeFilter& edge_filter, const NodeFilter& node_filter) {
  // Trivially finished already
  std::unordered_map<Location, PathLocation> searched;
  if(locations.empty())
    return searched;

  // Get the unique set of input locations
  std::unordered_set<Location> uniq_locations(locations.begin(), locations.end());
  std::vector<projector_t> pps;
  pps.reserve(uniq_locations.size());
  for (const auto& loc: uniq_locations)
    pps.emplace_back(loc, reader);

  // We keep pps sorted at each round to group the bins together
  // and project that every projection finished by just testing the
  // first one (finished projections are at the end when sorted).
  std::sort(pps.begin(), pps.end());
  while(pps.front().has_bin()) {
    auto range = find_best_range(pps);
    handle_bin(range.first, range.second, reader, edge_filter);
    std::sort(pps.begin(), pps.end());
  }

  // At this point we have candidates for each location so now we
  // need to go get the actual correlated location with edge_id etc.
  for (const auto& pp: pps) {
    if (pp.projection_found())
      auto inserted = searched.insert({pp.location, finalize(pp, reader, edge_filter)});
  }

  return searched;
}

}
}
