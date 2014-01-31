/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file yapf_road.cpp The road pathfinding. */

#include "../../stdafx.h"
#include "yapf.hpp"
#include "yapf_node_road.hpp"
#include "../../roadstop_base.h"

static int SlopeCost(const YAPFSettings *settings, TileIndex tile, TileIndex next)
{
	/* height of the center of the current tile */
	int x1 = TileX(tile) * TILE_SIZE;
	int y1 = TileY(tile) * TILE_SIZE;
	int z1 = GetSlopePixelZ(x1 + TILE_SIZE / 2, y1 + TILE_SIZE / 2);

	/* height of the center of the next tile */
	int x2 = TileX(next) * TILE_SIZE;
	int y2 = TileY(next) * TILE_SIZE;
	int z2 = GetSlopePixelZ(x2 + TILE_SIZE / 2, y2 + TILE_SIZE / 2);

	return (z2 - z1 > 1) ? settings->road_slope_penalty : 0;
}

/** return one tile cost */
static int OneTileCost(const YAPFSettings *settings, const PathPos &pos)
{
	int cost = 0;
	/* set base cost */
	if (IsDiagonalTrackdir(pos.td)) {
		cost += YAPF_TILE_LENGTH;
		switch (GetTileType(pos.tile)) {
			case TT_MISC:
				/* Increase the cost for level crossings */
				if (IsLevelCrossingTile(pos.tile)) {
					cost += settings->road_crossing_penalty;
				}
				break;

			case TT_STATION: {
				const RoadStop *rs = RoadStop::GetByTile(pos.tile, GetRoadStopType(pos.tile));
				if (IsDriveThroughStopTile(pos.tile)) {
					/* Increase the cost for drive-through road stops */
					cost += settings->road_stop_penalty;
					DiagDirection dir = TrackdirToExitdir(pos.td);
					if (!RoadStop::IsDriveThroughRoadStopContinuation(pos.tile, pos.tile - TileOffsByDiagDir(dir))) {
						/* When we're the first road stop in a 'queue' of them we increase
						 * cost based on the fill percentage of the whole queue. */
						const RoadStop::Entry *entry = rs->GetEntry(dir);
						cost += entry->GetOccupied() * settings->road_stop_occupied_penalty / entry->GetLength();
					}
				} else {
					/* Increase cost for filled road stops */
					cost += settings->road_stop_bay_occupied_penalty * (!rs->IsFreeBay(0) + !rs->IsFreeBay(1)) / 2;
				}
				break;
			}

			default:
				break;
		}
	} else {
		/* non-diagonal trackdir */
		cost = YAPF_TILE_CORNER_LENGTH + settings->road_curve_penalty;
	}
	return cost;
}


template <class Types>
class CYapfRoadT : public Types::Astar
{
public:
	typedef typename Types::Tpf Tpf;                     ///< pathfinder (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower; ///< track follower helper
	typedef typename Types::Astar::Node Node;            ///< this will be our node type
	typedef typename Node::Key Key;                      ///< key to hash tables

protected:
	const YAPFSettings *m_settings; ///< current settings (_settings_game.yapf)
	const RoadVehicle  *m_veh;      ///< vehicle that we are trying to drive

protected:
	/** to access inherited path finder */
	Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

	CYapfRoadT()
		: m_settings(&_settings_game.pf.yapf)
		, m_veh(NULL)
	{
	}

	/** return current settings (can be custom - company based - but later) */
	inline const YAPFSettings& PfGetSettings() const
	{
		return *m_settings;
	}

public:
	/**
	 * Called by YAPF to move from the given node to the next tile. For each
	 *  reachable trackdir on the new tile creates new node, initializes it
	 *  and adds it to the open list by calling Yapf().AddNewNode(n)
	 */
	inline void PfFollowNode(Node& old_node)
	{
		TrackFollower tf (m_veh);
		if (!tf.Follow(old_node.m_segment_last)) return;

		bool is_choice = !tf.m_new.is_single();
		uint initial_skipped_tiles = tf.m_tiles_skipped;
		PathPos pos = tf.m_new;
		for (TrackdirBits rtds = tf.m_new.trackdirs; rtds != TRACKDIR_BIT_NONE; rtds = KillFirstBit(rtds)) {
			pos.td = FindFirstTrackdir(rtds);
			Node *n = Types::Astar::CreateNewNode(&old_node, pos, is_choice);

			uint tiles = initial_skipped_tiles;
			int segment_cost = tiles * YAPF_TILE_LENGTH;

			/* start at pos and walk to the end of segment */
			n->m_segment_last = pos;
			tf.SetPos (pos);

			for (;;) {
				/* base tile cost depending on distance between edges */
				segment_cost += OneTileCost (&Yapf().PfGetSettings(), tf.m_new);

				/* we have reached the vehicle's destination - segment should end here to avoid target skipping */
				if (Yapf().PfDetectDestinationTile(tf.m_new)) break;

				/* stop if we have just entered the depot */
				if (IsRoadDepotTile(tf.m_new.tile) && tf.m_new.td == DiagDirToDiagTrackdir(ReverseDiagDir(GetGroundDepotDirection(tf.m_new.tile)))) {
					/* next time we will reverse and leave the depot */
					break;
				}

				/* if there are no reachable trackdirs on new tile, we have end of road */
				if (!tf.FollowNext()) break;

				/* if there are more trackdirs available & reachable, we are at the end of segment */
				if (!tf.m_new.is_single()) break;

				/* stop if RV is on simple loop with no junctions */
				if (tf.m_new == pos) return;

				/* if we skipped some tunnel tiles, add their cost */
				segment_cost += tf.m_tiles_skipped * YAPF_TILE_LENGTH;
				tiles += tf.m_tiles_skipped + 1;

				/* add hilly terrain penalty */
				assert (!tf.m_new.in_wormhole());
				segment_cost += SlopeCost(&Yapf().PfGetSettings(), tf.m_old.tile, tf.m_new.tile);

				/* add min/max speed penalties */
				int min_speed = 0;
				int max_veh_speed = m_veh->GetDisplayMaxSpeed();
				int max_speed = tf.GetSpeedLimit(&min_speed);
				if (max_speed < max_veh_speed) segment_cost += 1 * (max_veh_speed - max_speed);
				if (min_speed > max_veh_speed) segment_cost += 10 * (min_speed - max_veh_speed);

				/* move to the next tile */
				n->m_segment_last = tf.m_new;
				if (tiles > MAX_MAP_SIZE) break;
			}

			/* save also tile cost */
			n->m_cost = old_node.m_cost + segment_cost;

			/* evaluate the node */
			bool bValid = Yapf().PfCalcEstimate(*n);

			/* has the estimate callback marked this node as invalid? */
			if (!bValid) continue;

			/* detect the destination */
			if (Yapf().PfDetectDestination(*n)) {
				Types::Astar::FoundTarget(n);
			} else {
				Types::Astar::InsertNode(n);
			}
		}
	}

	/** call the node follower */
	static inline void Follow (Tpf *pf, Node *n)
	{
		pf->PfFollowNode(*n);
	}

	/**
	 * Main pathfinder routine:
	 *   - set startup node(s)
	 *   - main loop that stops if:
	 *      - the destination was found
	 *      - or the open list is empty (no route to destination).
	 *      - or the maximum amount of loops reached - m_max_search_nodes (default = 10000)
	 * @return true if the path was found
	 */
	inline bool FindPath(const RoadVehicle *v)
	{
		m_veh = v;

#ifndef NO_DEBUG_MESSAGES
		CPerformanceTimer perf;
		perf.Start();
#endif /* !NO_DEBUG_MESSAGES */

		bool bDestFound = Types::Astar::FindPath (Follow, PfGetSettings().max_search_nodes);

#ifndef NO_DEBUG_MESSAGES
		perf.Stop();
		if (_debug_yapf_level >= 2) {
			int t = perf.Get(1000000);
			_total_pf_time_us += t;

			if (_debug_yapf_level >= 3) {
				UnitID veh_idx = (m_veh != NULL) ? m_veh->unitnumber : 0;
				int cost = bDestFound ? Types::Astar::best->m_cost : -1;
				int dist = bDestFound ? Types::Astar::best->m_estimate - Types::Astar::best->m_cost : -1;

				DEBUG(yapf, 3, "[YAPFr]%c%4d- %d us - %d rounds - %d open - %d closed - CHR  0.0%% - C %d D %d - c0(sc0, ts0, o0) -- ",
					bDestFound ? '-' : '!', veh_idx, t, Types::Astar::num_steps, Types::Astar::OpenCount(), Types::Astar::ClosedCount(),
					cost, dist
				);
			}
		}
#endif /* !NO_DEBUG_MESSAGES */
		return bDestFound;
	}
};


template <class Types>
class CYapfDestinationAnyDepotRoadT
{
public:
	typedef typename Types::Tpf Tpf;                     ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::Astar::Node Node;            ///< this will be our node type
	typedef typename Node::Key Key;                      ///< key to hash tables

	/** to access inherited path finder */
	Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

	/** Called by YAPF to detect if node ends in the desired destination */
	inline bool PfDetectDestination(Node& n)
	{
		return IsRoadDepotTile(n.m_segment_last.tile);
	}

	inline bool PfDetectDestinationTile(const PathPos &pos)
	{
		return IsRoadDepotTile(pos.tile);
	}

	/**
	 * Called by YAPF to calculate cost estimate. Calculates distance to the destination
	 *  adds it to the actual cost from origin and stores the sum to the Node::m_estimate
	 */
	inline bool PfCalcEstimate(Node& n)
	{
		n.m_estimate = n.m_cost;
		return true;
	}
};


template <class Types>
class CYapfDestinationTileRoadT
{
public:
	typedef typename Types::Tpf Tpf;                     ///< the pathfinder class (derived from THIS class)
	typedef typename Types::TrackFollower TrackFollower;
	typedef typename Types::Astar::Node Node;            ///< this will be our node type
	typedef typename Node::Key Key;                      ///< key to hash tables

protected:
	TileIndex    m_destTile;
	StationID    m_dest_station;
	bool         m_bus;
	bool         m_non_artic;

public:
	void SetDestination(const RoadVehicle *v)
	{
		if (v->current_order.IsType(OT_GOTO_STATION)) {
			m_dest_station  = v->current_order.GetDestination();
			m_bus           = v->IsBus();
			m_destTile      = Station::Get(m_dest_station)->GetClosestTile(v->tile, m_bus ? STATION_BUS : STATION_TRUCK);
			m_non_artic     = !v->HasArticulatedPart();
		} else {
			m_dest_station  = INVALID_STATION;
			m_destTile      = v->dest_tile;
		}
	}

protected:
	/** to access inherited path finder */
	Tpf& Yapf()
	{
		return *static_cast<Tpf*>(this);
	}

public:
	/** Called by YAPF to detect if node ends in the desired destination */
	inline bool PfDetectDestination(Node& n)
	{
		return PfDetectDestinationTile(n.m_segment_last);
	}

	inline bool PfDetectDestinationTile(const PathPos &pos)
	{
		if (m_dest_station != INVALID_STATION) {
			return IsStationTile(pos.tile) &&
				GetStationIndex(pos.tile) == m_dest_station &&
				(m_bus ? IsBusStop(pos.tile) : IsTruckStop(pos.tile)) &&
				(m_non_artic || IsDriveThroughStopTile(pos.tile));
		}

		return pos.tile == m_destTile;
	}

	/**
	 * Called by YAPF to calculate cost estimate. Calculates distance to the destination
	 *  adds it to the actual cost from origin and stores the sum to the Node::m_estimate
	 */
	inline bool PfCalcEstimate(Node& n)
	{
		static const int dg_dir_to_x_offs[] = {-1, 0, 1, 0};
		static const int dg_dir_to_y_offs[] = {0, 1, 0, -1};
		if (PfDetectDestination(n)) {
			n.m_estimate = n.m_cost;
			return true;
		}

		TileIndex tile = n.m_segment_last.tile;
		DiagDirection exitdir = TrackdirToExitdir(n.m_segment_last.td);
		int x1 = 2 * TileX(tile) + dg_dir_to_x_offs[(int)exitdir];
		int y1 = 2 * TileY(tile) + dg_dir_to_y_offs[(int)exitdir];
		int x2 = 2 * TileX(m_destTile);
		int y2 = 2 * TileY(m_destTile);
		int dx = abs(x1 - x2);
		int dy = abs(y1 - y2);
		int dmin = min(dx, dy);
		int dxy = abs(dx - dy);
		int d = dmin * YAPF_TILE_CORNER_LENGTH + (dxy - 1) * (YAPF_TILE_LENGTH / 2);
		n.m_estimate = n.m_cost + d;
		assert(n.m_estimate >= n.m_parent->m_estimate);
		return true;
	}
};


template <class Tpf_, class TAstar>
struct CYapfRoad_TypesT
{
	typedef CYapfRoad_TypesT<Tpf_, TAstar> Types;

	typedef Tpf_                              Tpf;
	typedef CFollowTrackRoad                  TrackFollower;
	typedef TAstar                            Astar;
};

struct CYapfRoad1
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
	, CYapfRoadT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
	, CYapfDestinationTileRoadT<CYapfRoad_TypesT<CYapfRoad1, AstarRoadTrackDir> >
{
};

struct CYapfRoad2
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
	, CYapfRoadT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
	, CYapfDestinationTileRoadT<CYapfRoad_TypesT<CYapfRoad2, AstarRoadExitDir> >
{
};

struct CYapfRoadAnyDepot1
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
	, CYapfRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
	, CYapfDestinationAnyDepotRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot1, AstarRoadTrackDir> >
{
};

struct CYapfRoadAnyDepot2
	: CYapfBaseT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
	, CYapfRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
	, CYapfDestinationAnyDepotRoadT<CYapfRoad_TypesT<CYapfRoadAnyDepot2, AstarRoadExitDir> >
{
};


template <class Tpf>
static Trackdir ChooseRoadTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, bool &path_found)
{
	Tpf pf;

	/* set origin and destination nodes */
	TrackdirBits trackdirs = TrackStatusToTrackdirBits(GetTileRoadStatus(tile, v->compatible_roadtypes)) & DiagdirReachesTrackdirs(enterdir);
	assert (!HasAtMostOneBit(trackdirs));

	PathPos pos;
	pos.tile = tile;
	for (TrackdirBits tdb = trackdirs; tdb != TRACKDIR_BIT_NONE; tdb = KillFirstBit(tdb)) {
		pos.td = FindFirstTrackdir(tdb);
		pf.InsertInitialNode (pf.CreateNewNode (NULL, pos, true));
	}

	pf.SetDestination(v);

	/* find the best path */
	path_found = pf.FindPath(v);

	/* if path not found - return INVALID_TRACKDIR */
	typename Tpf::TT::Astar::Node *n = pf.GetBestNode();
	if (n == NULL) return INVALID_TRACKDIR;

	/* path was found or at least suggested
	 * walk through the path back to its origin */
	while (n->m_parent != NULL) {
		n = n->m_parent;
	}

	/* return trackdir from the best origin node (one of start nodes) */
	assert(n->GetPos().tile == tile);
	return n->GetPos().td;
}

Trackdir YapfRoadVehicleChooseTrack(const RoadVehicle *v, TileIndex tile, DiagDirection enterdir, TrackdirBits trackdirs, bool &path_found)
{
	/* Handle special case - when next tile is destination tile.
	 * However, when going to a station the (initial) destination
	 * tile might not be a station, but a junction, in which case
	 * this method forces the vehicle to jump in circles. */
	if (tile == v->dest_tile && !v->current_order.IsType(OT_GOTO_STATION)) {
		/* choose diagonal trackdir reachable from enterdir */
		return DiagDirToDiagTrackdir(enterdir);
	}

	/* default is YAPF type 2 */
	typedef Trackdir (*PfnChooseRoadTrack)(const RoadVehicle*, TileIndex, DiagDirection, bool &path_found);
	PfnChooseRoadTrack pfnChooseRoadTrack = &ChooseRoadTrack<CYapfRoad2>; // default: ExitDir, allow 90-deg

	/* check if non-default YAPF type should be used */
	if (_settings_game.pf.yapf.disable_node_optimization) {
		pfnChooseRoadTrack = &ChooseRoadTrack<CYapfRoad1>; // Trackdir, allow 90-deg
	}

	Trackdir td_ret = pfnChooseRoadTrack(v, tile, enterdir, path_found);
	return (td_ret != INVALID_TRACKDIR) ? td_ret : (Trackdir)FindFirstBit2x64(trackdirs);
}


template <class Tpf>
static TileIndex FindNearestDepot(const RoadVehicle *v, const PathPos &pos, int max_distance)
{
	Tpf pf;

	/* set origin and destination nodes */
	pf.InsertInitialNode (pf.CreateNewNode (NULL, pos, false));

	/* find the best path */
	if (!pf.FindPath(v)) return INVALID_TILE;

	/* some path found; get found depot tile */
	typename Tpf::TT::Astar::Node *n = pf.GetBestNode();

	if (max_distance > 0 && n->m_cost > max_distance * YAPF_TILE_LENGTH) return INVALID_TILE;

	return n->m_segment_last.tile;
}

TileIndex YapfRoadVehicleFindNearestDepot(const RoadVehicle *v, uint max_distance)
{
	PathPos pos = v->GetPos();
	if ((TrackStatusToTrackdirBits(GetTileRoadStatus(pos.tile, v->compatible_roadtypes)) & TrackdirToTrackdirBits(pos.td)) == 0) {
		return false;
	}

	/* default is YAPF type 2 */
	typedef TileIndex (*PfnFindNearestDepot)(const RoadVehicle*, const PathPos&, int);
	PfnFindNearestDepot pfnFindNearestDepot = &FindNearestDepot<CYapfRoadAnyDepot2>;

	/* check if non-default YAPF type should be used */
	if (_settings_game.pf.yapf.disable_node_optimization) {
		pfnFindNearestDepot = &FindNearestDepot<CYapfRoadAnyDepot1>; // Trackdir, allow 90-deg
	}

	return pfnFindNearestDepot(v, pos, max_distance);
}
