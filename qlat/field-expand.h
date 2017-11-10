// vim: set ts=2 sw=2 expandtab:

#pragma once

#include <qlat/config.h>
#include <qlat/utils.h>

#include <mpi.h>

#include <array>
#include <map>
#include <set>
#include <vector>

QLAT_START_NAMESPACE

struct CommMarks : Field<int8_t>
{
  virtual const std::string& cname()
  {
    static const std::string s = "CommMarks";
    return s;
  }
};

typedef void (*SetMarksField)(CommMarks& marks, const Geometry& geo, const std::string& tag);

inline void set_marks_field_all(CommMarks& marks, const Geometry& geo, const std::string& tag)
  // tag is not used
{
  TIMER_VERBOSE("set_marks_field_all");
  marks.init();
  marks.init(geo);
#pragma omp parallel for
  for (long offset = 0; offset < geo.local_volume_expanded() * geo.multiplicity; ++offset) {
    const Coordinate xl = geo.coordinate_from_offset(offset);
    if (not geo.is_local(xl)) {
      marks.get_elem(offset) = 1;
    }
  }
}

inline void set_marks_field_1(CommMarks& marks, const Geometry& geo, const std::string& tag)
  // tag is not used
{
  TIMER_VERBOSE("set_marks_field_1");
  marks.init();
  marks.init(geo);
#pragma omp parallel for
  for (long index = 0; index < geo.local_volume(); ++index) {
    const Coordinate xl = geo.coordinate_from_index(index);
    for (int dir = -4; dir < 4; ++dir) {
      const Coordinate xl1 = coordinate_shifts(xl, dir);
      if (geo.is_on_node(xl) and !geo.is_local(xl)) {
        Vector<int8_t> v = marks.get_elems(xl);
        for (int m = 0; m < geo.multiplicity; ++m) {
          v[m] = 1;
        }
      }
    }
  }
}

struct CommPackInfo
{
  long offset;
  long buffer_idx;
  long size;
};

struct CommMsgInfo
{
  int id_node;
  long buffer_idx;
  long size;
};

struct CommPlan
{
  long total_send_size; // send buffer size
  std::vector<CommMsgInfo> send_msg_infos;
  std::vector<CommPackInfo> send_pack_infos;
  long total_recv_size; // recv buffer size
  std::vector<CommMsgInfo> recv_msg_infos;
  std::vector<CommPackInfo> recv_pack_infos;
};

struct CommPlanKey
{
  SetMarksField set_marks_field;
  std::string tag;
  Geometry geo;
};

inline bool operator<(const CommPlanKey& x, const CommPlanKey& y)
{
  if (x.set_marks_field < y.set_marks_field) {
    return true;
  } else if (y.set_marks_field < x.set_marks_field) {
    return false;
  } else if (x.tag < y.tag) {
    return true;
  } else if (y.tag < x.tag) {
    return false;
  } else {
    const std::string xgeo = show(x.geo);
    const std::string ygeo = show(y.geo);
    return xgeo < ygeo;
  }
}

inline void g_offset_id_node_from_offset(long& g_offset, int& id_node, const long offset, const Geometry& geo)
{
  const Coordinate total_site = geo.total_site();
  const Coordinate xl = geo.coordinate_from_offset(offset);
  const Coordinate xg = regular_coordinate(geo.coordinate_g_from_l(xl), total_site);
  const Coordinate coor_node = xg / geo.node_site;
  id_node = index_from_coordinate(coor_node, geo.geon.size_node);
  g_offset = index_from_coordinate(xg, total_site) * geo.multiplicity + offset % geo.multiplicity;
}

inline long offset_from_g_offset(const long g_offset, const Geometry& geo)
{
  const Coordinate total_site = geo.total_site();
  const Coordinate xg = coordinate_from_index(g_offset/geo.multiplicity, total_site);
  Coordinate xl = regular_coordinate(geo.coordinate_l_from_g(xg), total_site);
  for (int mu = 0; mu < DIMN; ++mu) {
    while (xl[mu] >= geo.node_site[mu] + geo.expansion_right[mu]) {
      xl[mu] -= total_site[mu];
    }
    while (xl[mu] < -geo.expansion_left[mu]) {
      xl[mu] += total_site[mu];
    }
  }
  qassert(geo.is_on_node(xl));
  return geo.offset_from_coordinate(xl) + g_offset % geo.multiplicity;
}

inline CommPlan make_comm_plan(const CommMarks& marks)
{
  TIMER_VERBOSE("make_comm_plan");
  const Geometry& geo = marks.geo;
  CommPlan ret;
  ret.total_send_size = 0;
  ret.total_recv_size = 0;
  //
  std::map<int,std::vector<long> > src_id_node_g_offsets; // src node id ; vector of g_offset
  for (long offset = 0; offset < geo.local_volume_expanded() * geo.multiplicity; ++offset) {
    const int8_t r = marks.get_elem(offset);
    if (r != 0) {
      int id_node;
      long g_offset;
      g_offset_id_node_from_offset(g_offset, id_node, offset, geo);
      if (id_node != get_id_node()) {
        qassert(0 <= id_node and id_node < get_num_node());
        src_id_node_g_offsets[id_node].push_back(g_offset);
      }
    }
  }
  //
  std::vector<long> src_id_node_count(get_num_node(), 0); // number of total send pkgs for each node
  {
    long count = 0;
    for (std::map<int,std::vector<long> >::const_iterator it = src_id_node_g_offsets.cbegin(); it != src_id_node_g_offsets.cend(); ++it) {
      src_id_node_count[it->first] += 1;
      CommMsgInfo cmi;
      cmi.id_node = it->first;
      cmi.buffer_idx = count;
      cmi.size = it->second.size();
      ret.recv_msg_infos.push_back(cmi);
      count += cmi.size;
    }
    ret.total_recv_size = count;
    // ret.total_recv_size finish
    // ret.recv_msg_infos finish
  }
  glb_sum(get_data(src_id_node_count));
  ret.send_msg_infos.resize(src_id_node_count[get_id_node()]);
  //
  std::map<int,std::vector<long> > dst_id_node_g_offsets; // dst node id ; vector of g_offset
  {
    std::vector<MPI_Request> send_reqs(src_id_node_g_offsets.size());
    std::vector<MPI_Request> recv_reqs(ret.send_msg_infos.size());
    {
      const int mpi_tag = 8;
      std::vector<CommMsgInfo> send_send_msg_infos(src_id_node_g_offsets.size());
      int k = 0;
      for (std::map<int,std::vector<long> >::const_iterator it = src_id_node_g_offsets.cbegin(); it != src_id_node_g_offsets.cend(); ++it) {
        CommMsgInfo& cmi = send_send_msg_infos[k];
        cmi.id_node = get_id_node();
        cmi.buffer_idx = 0;
        cmi.size = it->second.size();
        MPI_Isend(&cmi, sizeof(CommMsgInfo), MPI_BYTE, it->first,
            mpi_tag, get_comm(), &send_reqs[k]);
        k += 1;
      }
      for (int i = 0; i < ret.send_msg_infos.size(); ++i) {
        CommMsgInfo& cmi = ret.send_msg_infos[i];
        MPI_Recv(&cmi, sizeof(CommMsgInfo), MPI_BYTE, MPI_ANY_SOURCE,
            mpi_tag, get_comm(), MPI_STATUS_IGNORE);
        dst_id_node_g_offsets[cmi.id_node].resize(cmi.size);
      }
      MPI_Waitall(send_reqs.size(), send_reqs.data(), MPI_STATUS_IGNORE);
    }
    {
      const int mpi_tag = 9;
      int k = 0;
      for (std::map<int,std::vector<long> >::const_iterator it = src_id_node_g_offsets.cbegin(); it != src_id_node_g_offsets.cend(); ++it) {
        MPI_Isend((void*)it->second.data(), it->second.size(), MPI_LONG, it->first,
            mpi_tag, get_comm(), &send_reqs[k]);
        k += 1;
      }
      k = 0;
      long count = 0;
      for (std::map<int,std::vector<long> >::iterator it = dst_id_node_g_offsets.begin(); it != dst_id_node_g_offsets.end(); ++it) {
        CommMsgInfo& cmi = ret.send_msg_infos[k];
        cmi.id_node = it->first;
        cmi.buffer_idx = count;
        cmi.size = it->second.size();
        count += cmi.size;
        MPI_Irecv(it->second.data(), it->second.size(), MPI_LONG, it->first,
            mpi_tag, get_comm(), &recv_reqs[k]);
        k += 1;
      }
      ret.total_send_size = count;
      // ret.total_send_size finish
      // ret.send_msg_infos finish
      MPI_Waitall(send_reqs.size(), send_reqs.data(), MPI_STATUS_IGNORE);
      MPI_Waitall(recv_reqs.size(), recv_reqs.data(), MPI_STATUS_IGNORE);
    }
  }
  {
    long current_buffer_idx = 0;
    int k = 0;
    for (std::map<int,std::vector<long> >::const_iterator it = src_id_node_g_offsets.cbegin(); it != src_id_node_g_offsets.cend(); ++it) {
      const int src_id_node = it->first;
      const std::vector<long>& g_offsets = it->second;
      qassert(src_id_node == ret.recv_msg_infos[k].id_node);
      qassert(current_buffer_idx == ret.recv_msg_infos[k].buffer_idx);
      qassert(g_offsets.size() == ret.recv_msg_infos[k].size);
      long current_offset = -1;
      for (long i = 0; i < g_offsets.size(); ++i) {
        const long g_offset = g_offsets[i];
        const long offset = offset_from_g_offset(g_offset, geo);
        if (offset != current_offset) {
          CommPackInfo cpi;
          cpi.offset = offset;
          cpi.buffer_idx = current_buffer_idx;
          cpi.size = 1;
          ret.recv_pack_infos.push_back(cpi);
          current_offset = offset + 1;
          current_buffer_idx += 1;
        } else {
          CommPackInfo& cpi = ret.recv_pack_infos.back();
          cpi.size += 1;
          current_offset = offset + 1;
          current_buffer_idx += 1;
        }
      }
      k += 1;
    }
  }
  {
    long current_buffer_idx = 0;
    int k = 0;
    for (std::map<int,std::vector<long> >::const_iterator it = dst_id_node_g_offsets.cbegin(); it != dst_id_node_g_offsets.cend(); ++it) {
      const int dst_id_node = it->first;
      const std::vector<long>& g_offsets = it->second;
      qassert(dst_id_node == ret.send_msg_infos[k].id_node);
      qassert(current_buffer_idx == ret.send_msg_infos[k].buffer_idx);
      qassert(g_offsets.size() == ret.send_msg_infos[k].size);
      long current_offset = -1;
      for (long i = 0; i < g_offsets.size(); ++i) {
        const long g_offset = g_offsets[i];
        const long offset = offset_from_g_offset(g_offset, geo);
        if (offset != current_offset) {
          CommPackInfo cpi;
          cpi.offset = offset;
          cpi.buffer_idx = current_buffer_idx;
          cpi.size = 1;
          ret.send_pack_infos.push_back(cpi);
          current_offset = offset + 1;
          current_buffer_idx += 1;
        } else {
          CommPackInfo& cpi = ret.send_pack_infos.back();
          cpi.size += 1;
          current_offset = offset + 1;
          current_buffer_idx += 1;
        }
      }
      k += 1;
    }
  }
  return ret;
}

inline CommPlan make_comm_plan(const CommPlanKey& cpk)
{
  CommMarks marks;
  cpk.set_marks_field(marks, cpk.geo, cpk.tag);
  return make_comm_plan(marks);
}

inline Cache<CommPlanKey,CommPlan>& get_comm_plan_cache()
{
  static Cache<CommPlanKey,CommPlan> cache("CommPlanCache", 32);
  return cache;
}

inline const CommPlan& get_comm_plan(const CommPlanKey& cpk)
{
  if (!get_comm_plan_cache().has(cpk)) {
    get_comm_plan_cache()[cpk] = make_comm_plan(cpk);
  }
  return get_comm_plan_cache()[cpk];
}

inline const CommPlan& get_comm_plan(const SetMarksField& set_marks_field, const std::string& tag, const Geometry& geo)
{
  CommPlanKey cpk;
  cpk.set_marks_field = set_marks_field;
  cpk.tag = tag;
  cpk.geo = geo;
  return get_comm_plan(cpk);
}

template <class M>
inline refresh_expanded(Field<M>& f, const CommPlan& plan)
{
  TIMER_FLOPS("refresh_expanded");
  timer.flops += (plan.total_recv_size + plan.total_send_size) * sizeof(M) / 2;
  std::vector<M> send_buffer(plan.total_send_size);
  std::vector<M> recv_buffer(plan.total_recv_size);
#pragma omp parallel for
  for (long i = 0; i < plan.send_pack_infos.size(); ++i) {
    const CommPackInfo& cpi = plan.send_pack_infos[i];
    memcpy(&send_buffer[cpi.buffer_idx], &f.get_elem(cpi.offset), cpi.size * sizeof(M));
  }
  {
    sync_node();
    TIMER_FLOPS("refresh_expanded-comm");
    timer.flops += (plan.total_recv_size + plan.total_send_size) * sizeof(M) / 2;
    std::vector<MPI_Request> send_reqs(plan.send_msg_infos.size());
    std::vector<MPI_Request> recv_reqs(plan.recv_msg_infos.size());
    {
      TIMER("refresh_expanded-comm-init");
      const int mpi_tag = 10;
      for (size_t i = 0; i < plan.send_msg_infos.size(); ++i) {
        const CommMsgInfo& cmi = plan.send_msg_infos[i];
        MPI_Isend(&send_buffer[cmi.buffer_idx], cmi.size * sizeof(M), MPI_BYTE, cmi.id_node,
            mpi_tag, get_comm(), &send_reqs[i]);
      }
      for (size_t i = 0; i < plan.recv_msg_infos.size(); ++i) {
        const CommMsgInfo& cmi = plan.recv_msg_infos[i];
        MPI_Irecv(&recv_buffer[cmi.buffer_idx], cmi.size * sizeof(M), MPI_BYTE, cmi.id_node,
            mpi_tag, get_comm(), &recv_reqs[i]);
      }
    }
    MPI_Waitall(recv_reqs.size(), recv_reqs.data(), MPI_STATUS_IGNORE);
    MPI_Waitall(send_reqs.size(), send_reqs.data(), MPI_STATUS_IGNORE);
    sync_node();
  }
#pragma omp parallel for
  for (long i = 0; i < plan.recv_pack_infos.size(); ++i) {
    const CommPackInfo& cpi = plan.recv_pack_infos[i];
    memcpy(&f.get_elem(cpi.offset), &recv_buffer[cpi.buffer_idx], cpi.size * sizeof(M));
  }
}

template <class M>
inline refresh_expanded(Field<M>& f, const SetMarksField& set_marks_field = set_marks_field_all, const std::string& tag = "")
{
  const CommPlan& plan = get_comm_plan(set_marks_field, tag, f.geo);
  refresh_expanded(f, plan);
}



template <class M>
void refresh_expanded_(Field<M>& field_comm)
{
  TIMER("refresh_expanded");
  // tested for expansion = 2 case.
  std::map<Coordinate, std::vector<M> > send_map;
  std::map<Coordinate, int> send_map_consume;

  Coordinate pos; // coordinate position of a site relative to this node
  Coordinate local_pos; // coordinate position of a site relative to its home node
  Coordinate node_pos; // home node coordinate of a site in node space

  // populate send_map with the data that we need to send to other nodes
  long record_size = field_comm.geo.local_volume_expanded();
  for(long record = 0; record < record_size; record++){
    pos = field_comm.geo.coordinateFromRecord(record);
    if(field_comm.geo.is_local(pos)) continue;
    for(int mu = 0; mu < DIMN; mu++){
      local_pos[mu] = pos[mu] % field_comm.geo.node_site[mu];
      node_pos[mu] = pos[mu] / field_comm.geo.node_site[mu];
      if(local_pos[mu] < 0){
        local_pos[mu] += field_comm.geo.node_site[mu];
        node_pos[mu]--;
      }
    }
    std::vector<M> &vec = send_map[node_pos];
    for(int mu = 0; mu < field_comm.geo.multiplicity; mu++)
      vec.push_back(field_comm.get_elems_const(local_pos)[mu]);
  }

  std::vector<M> recv_vec;
  // will store data received from other nodes
  // Iterate over all the nodes to which we need to send data.
  // We ultimately copy the received data into the corresponding
  // value of sendmap.
  typename std::map<Coordinate, std::vector<M> >::iterator it;

  // pure communication

  for(it = send_map.begin(); it != send_map.end(); it++){
    node_pos = it->first;
    std::vector<M> &send_vec = it->second;
    long size = send_vec.size();
    size_t size_bytes = size * sizeof(M);
    recv_vec.resize(std::max((long)2500, size));

    M *send = send_vec.data();
    M *recv = recv_vec.data();

    Coordinate coor_this, coort, coorf;
    int id_this, idt, idf;
    // assuming periodic boundary condition. maybe need some fixing?
    id_this = get_id_node();
    coor_this = qlat::coordinate_from_index(id_this, \
        field_comm.geo.geon.size_node);
    coort = coor_this - node_pos; 
    regularize_coordinate(coort, field_comm.geo.geon.size_node);
    coorf = coor_this + node_pos;
    regularize_coordinate(coorf, field_comm.geo.geon.size_node);

    idt = qlat::index_from_coordinate(coort, field_comm.geo.geon.size_node);
    idf = qlat::index_from_coordinate(coorf, field_comm.geo.geon.size_node);

    MPI_Request req;
    MPI_Isend((void*)send, size_bytes, MPI_BYTE, idt, 0, get_comm(), &req);
    const int ret = MPI_Recv((void*)recv, size_bytes, MPI_BYTE, \
        idf, 0, get_comm(), MPI_STATUS_IGNORE);
    MPI_Wait(&req, MPI_STATUS_IGNORE);
    qassert(!ret);

    memcpy(send, recv, size_bytes);

    send_map_consume[node_pos] = 0;
  }
  // Now send_map[node_pos] is the vector of data recieved from the node
  // pointed to by key.
  for(long record = 0; record < record_size; record++){
    pos = field_comm.geo.coordinateFromRecord(record);
    if(field_comm.geo.is_local(pos)) continue;
    for(int mu = 0; mu < DIMN; mu++){
      local_pos[mu] = pos[mu] % field_comm.geo.node_site[mu];
      node_pos[mu] = pos[mu] / field_comm.geo.node_site[mu];
      if(local_pos[mu] < 0){
        local_pos[mu] += field_comm.geo.node_site[mu];
        node_pos[mu]--;
      }
    }
    // send_map_consume[key] keeps track of our progress in consuming the
    // received data in sendmap[key], so that we know which offset of
    // send_map[node_pos] corresponds to which site.
    int consume = send_map_consume[node_pos];
    std::vector<M> &vec = send_map[node_pos];
    for(int mu = 0; mu < field_comm.geo.multiplicity; mu++){
      field_comm.get_elems(pos)[mu] = vec[consume];
      consume++;
    }
    send_map_consume[node_pos] = consume;
  }
}

QLAT_END_NAMESPACE
