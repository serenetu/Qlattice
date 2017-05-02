#pragma once

#include <qlat/config.h>
#include <qlat/utils.h>

#include <array>

QLAT_START_NAMESPACE

inline MPI_Comm& get_qlat_comm()
{
  static MPI_Comm comm;
  return comm;
}

inline MPI_Comm*& get_comm_ptr()
{
  static MPI_Comm* p_comm = &get_qlat_comm();
  return p_comm;
}

inline MPI_Comm& get_comm()
{
  return *get_comm_ptr();
}

struct GeometryNode
{
  bool initialized;
  // About node geometry.
  int num_node;
  // num_node = size_node[0] * size_node[1] * size_node[2] * size_node[3]
  int id_node;
  // id_node = get_id_node()
  // 0 <= id_node < num_node
  Coordinate size_node;
  Coordinate coor_node;
  // 0 <= coor_node[i] < size_node[i]
  //
  inline void init();
  //
  GeometryNode(const bool initialize = false)
  {
    memset(this, 0, sizeof(GeometryNode));
    if (initialize) {
      init();
    }
  }
};

inline bool operator==(const GeometryNode& geon1, const GeometryNode& geon2)
{
  return geon1.initialized == geon2.initialized
    && geon1.num_node == geon2.num_node
    && geon1.id_node == geon2.id_node
    && geon1.size_node == geon2.size_node
    && geon1.coor_node == geon2.coor_node;
}

inline bool operator!=(const GeometryNode& geon1, const GeometryNode& geon2)
{
  return !(geon1 == geon2);
}

inline int id_node_from_coor_node(const Coordinate& coor_node)
{
  Coordinate size_node;
  Coordinate periods;
  Coordinate coor_node_check;
  MPI_Cart_get(get_comm(), DIMN, size_node.data(), periods.data(), coor_node_check.data());
  return index_from_coordinate(coor_node, size_node);
}

inline Coordinate coor_node_from_id_node(int id_node)
{
  Coordinate size_node;
  Coordinate periods;
  Coordinate coor_node_check;
  MPI_Cart_get(get_comm(), DIMN, size_node.data(), periods.data(), coor_node_check.data());
  return coordinate_from_index(id_node, size_node);
}

inline void GeometryNode::init()
{
  if (initialized) {
    return;
  }
#ifdef USE_MULTI_NODE
  MPI_Comm_size(get_comm(), &num_node);
  MPI_Comm_rank(get_comm(), &id_node);
  int ndims;
  MPI_Cartdim_get(get_comm(), &ndims);
  qassert(DIMN == ndims);
  Coordinate periods;
  Coordinate coor_node_check;
  MPI_Cart_get(get_comm(), DIMN, size_node.data(), periods.data(), coor_node_check.data());
  for (int i = 0; i < DIMN; ++i) {
    qassert(0 != periods[i]);
  }
  coor_node = coordinate_from_index(id_node, size_node);
  for (int i = 0; i < DIMN; ++i) {
    qassert(0 != periods[i]);
    // qassert(coor_node_check[i] == coor_node[i]);
  }
  qassert(size_node[0] * size_node[1] * size_node[2] * size_node[3] == num_node);
  qassert(id_node_from_coor_node(coor_node) == id_node);
#else
  num_node = 1;
  id_node = 0;
  for (int i = 0; i < DIMN; ++i) {
    size_node[i] = 1;
    coor_node[i] = 0;
  }
#endif
  initialized = true;
}

inline bool is_initialized(const GeometryNode& geon)
{
  return geon.initialized;
}

inline void init(GeometryNode& geon)
{
  geon.init();
}

inline const GeometryNode& get_geometry_node()
{
  static GeometryNode geon(true);
  return geon;
}

inline int get_num_node()
{
  return get_geometry_node().num_node;
}

inline int get_id_node()
{
  return get_geometry_node().id_node;
}

inline const Coordinate& get_size_node()
{
  return get_geometry_node().size_node;
}

inline const Coordinate& get_coor_node()
{
  return get_geometry_node().coor_node;
}

struct GeometryNodeNeighbor
{
  int dest[2][DIMN];
  // dest[dir][mu]
  // dir = 0, 1 for Plus dir or Minus dir
  // 0 <= mu < 4 for different directions
  //
  void init()
  {
    const Coordinate& coor_node = get_geometry_node().coor_node;
    const Coordinate& size_node = get_geometry_node().size_node;
    for (int mu = 0; mu < DIMN; ++mu) {
      Coordinate coor;
      coor = coor_node;
      ++coor[mu];
      regularize_coordinate(coor, size_node);
      dest[0][mu] = id_node_from_coor_node(coor);
      coor = coor_node;
      --coor[mu];
      regularize_coordinate(coor, size_node);
      dest[1][mu] = id_node_from_coor_node(coor);
    }
  }
  //
  GeometryNodeNeighbor()
  {
  }
  GeometryNodeNeighbor(bool initialize)
  {
    if (initialize) {
      init();
    }
  }
};

inline const GeometryNodeNeighbor& get_geometry_node_neighbor()
{
  static GeometryNodeNeighbor geonb(true);
  return geonb;
}

template <class M>
inline std::vector<char> pad_flag_data(const int64_t flag, const M& data)
{
  std::vector<char> fdata(8 + sizeof(M), (char)0);
  std::memcpy(&fdata[0], &flag, sizeof(int64_t));
  std::memcpy(&fdata[8], &data, sizeof(M));
  return fdata;
}

template <class M>
inline void extract_flag_data(int64_t& flag, M& data, const std::vector<char>& fdata)
{
  flag = fdata[0];
  std::memcpy(&flag, &fdata[0], sizeof(int64_t));
  std::memcpy(&data, &fdata[8], sizeof(M));
}

template <class N>
inline int receive_job(int64_t& flag, N& data, const int root = 0)
{
  const int tag = 3;
  const int count = sizeof(int64_t) + sizeof(N);
  std::vector<char> fdata(count, (char)0);
  int ret = MPI_Recv(fdata.data(), count, MPI_BYTE, root, tag, get_comm(), MPI_STATUS_IGNORE);
  extract_flag_data(flag, data, fdata);
  return ret;
}

template <class M>
inline int send_result(const int64_t flag, const M& data, const int root = 0)
{
  const int tag = 2;
  std::vector<char> fdata = pad_flag_data(flag, data);
  return MPI_Send(fdata.data(), fdata.size(), MPI_BYTE, root, tag, get_comm());
}

template <class N>
inline int send_job(const int64_t flag, const N& data, const int dest)
{
  const int tag = 3;
  std::vector<char> fdata = pad_flag_data(flag, data);
  return MPI_Send(fdata.data(), fdata.size(), MPI_BYTE, dest, tag, get_comm());
}

template <class M>
inline int receive_result(int& source, int64_t& flag, M& result)
{
  const int tag = 2;
  const int count = sizeof(int64_t) + sizeof(M);
  std::vector<char> fdata(count, (char)0);
  MPI_Status status;
  const int ret = MPI_Recv(fdata.data(), fdata.size(), MPI_BYTE, MPI_ANY_SOURCE, tag, get_comm(), &status);
  source = status.MPI_SOURCE;
  extract_flag_data(flag, result, fdata);
  return ret;
}

template <class M>
int get_data_dir(Vector<M> recv, const Vector<M>& send, const int dir)
  // dir = 0, 1 for Plus dir or Minus dir
{
  TIMER_FLOPS("get_data_dir");
  const int tag = 0;
  qassert(recv.size() == send.size());
  const long size = recv.size()*sizeof(M);
  timer.flops += size;
#ifdef USE_MULTI_NODE
  const int self_ID = get_id_node();
  const int idf = (self_ID + 1 - 2 * dir + get_num_node()) % get_num_node();
  const int idt = (self_ID - 1 + 2 * dir + get_num_node()) % get_num_node();;
  MPI_Request req;
  MPI_Isend((void*)send.data(), size, MPI_BYTE, idt, tag, get_comm(), &req);
  const int ret = MPI_Recv(recv.data(), size, MPI_BYTE, idf, tag, get_comm(), MPI_STATUS_IGNORE);
  MPI_Wait(&req, MPI_STATUS_IGNORE);
  return ret;
#else
  memcpy(recv.data(), send.data(), size);
  return 0;
#endif
}

template <class M>
int get_data_dir_mu(Vector<M> recv, const Vector<M>& send, const int dir, const int mu)
  // dir = 0, 1 for Plus dir or Minus dir
  // 0 <= mu < 4 for different directions
{
  TIMER_FLOPS("get_data_dir_mu");
  const int tag = 1;
  qassert(recv.size() == send.size());
  const long size = recv.size()*sizeof(M);
  timer.flops += size;
#ifdef USE_MULTI_NODE
  const GeometryNodeNeighbor& geonb = get_geometry_node_neighbor();
  const int idf = geonb.dest[dir][mu];
  const int idt = geonb.dest[1-dir][mu];
  MPI_Request req;
  MPI_Isend((void*)send.data(), size, MPI_BYTE, idt, tag, get_comm(), &req);
  const int ret = MPI_Recv(recv.data(), size, MPI_BYTE, idf, tag, get_comm(), MPI_STATUS_IGNORE);
  MPI_Wait(&req, MPI_STATUS_IGNORE);
  return ret;
#else
  memcpy(recv.data(), send.data(), size);
  return 0;
#endif
}

template <class M>
int get_data_plus_mu(Vector<M> recv, const Vector<M>& send, const int mu)
{
  return get_data_dir_mu(recv, send, 0, mu);
}

template <class M>
int get_data_minus_mu(Vector<M> recv, const Vector<M>& send, const int mu)
{
  return get_data_dir_mu(recv, send, 1, mu);
}

inline int glb_sum(Vector<double> recv, const Vector<double>& send)
{
  qassert(recv.size() == send.size());
#ifdef USE_MULTI_NODE
  return MPI_Allreduce((double*)send.data(), recv.data(), recv.size(), MPI_DOUBLE, MPI_SUM, get_comm());
#else
  memmove(recv.data(), send.data(), recv.size()* sizeof(double));
  return 0;
#endif
}

inline int glb_sum(Vector<Complex> recv, const Vector<Complex>& send)
{
  return glb_sum(Vector<double>((double*)recv.data(), recv.size() * 2), Vector<double>((double*)send.data(), send.size() * 2));
}

inline int glb_sum(Vector<long> recv, const Vector<long>& send)
{
  qassert(recv.size() == send.size());
#ifdef USE_MULTI_NODE
  return MPI_Allreduce((long*)send.data(), recv.data(), recv.size(), MPI_LONG, MPI_SUM, get_comm());
#else
  memmove(recv.data(), send.data(), recv.size()* sizeof(long));
  return 0;
#endif
}

inline int glb_sum(Vector<double> vec)
{
  std::vector<double> tmp(vec.size());
  assign(tmp, vec);
  return glb_sum(vec, tmp);
}

inline int glb_sum(Vector<Complex> vec)
{
  std::vector<Complex> tmp(vec.size());
  assign(tmp, vec);
  return glb_sum(vec, tmp);
}

inline int glb_sum(Vector<long> vec)
{
  std::vector<long> tmp(vec.size());
  assign(tmp, vec);
  return glb_sum(vec, tmp);
}

inline int glb_sum(double& x)
{
  return glb_sum(Vector<double>(x));
}

inline int glb_sum(long& x)
{
  return glb_sum(Vector<long>(x));
}

inline int glb_sum(Complex& c)
{
  std::vector<double> vec(2);
  vec[0] = c.real();
  vec[1] = c.imag();
  const int ret = glb_sum(Vector<double>(vec));
  c.real(vec[0]);
  c.imag(vec[1]);
  return ret;
}

template <class M>
inline int glb_sum_double_vec(Vector<M> x)
{
  return glb_sum(Vector<double>((double*)x.data(), x.data_size()/sizeof(double)));
}

template <class M>
inline int glb_sum_long_vec(Vector<M> x)
{
  return glb_sum(Vector<long>((long*)x.data(), x.data_size()/sizeof(long)));
}

template <class M>
inline int glb_sum_double(M& x)
{
  return glb_sum(Vector<double>((double*)&x, sizeof(M)/sizeof(double)));
}

template <class M>
inline int glb_sum_long(M& x)
{
  return glb_sum(Vector<long>((long*)&x, sizeof(M)/sizeof(long)));
}

template <class M>
void all_gather(Vector<M> recv, const Vector<M>& send)
{
  qassert(recv.size() == send.size() * get_num_node());
  const long sendsize = send.size() * sizeof(M);
#ifdef USE_MULTI_NODE
  MPI_Allgather((void*)send.data(), send.data_size(), MPI_BYTE, (void*)recv.data(), send.data_size(), MPI_BYTE, get_comm());
#else
  memmove(recv, send, sendsize);
#endif
}

template <class M>
inline void bcast(Vector<M> recv, const int root = 0)
{
#ifdef USE_MULTI_NODE
  MPI_Bcast((void*)recv.data(), recv.data_size(), MPI_BYTE, root, get_comm());
#endif
}

template <class M>
inline void concat_vector(std::vector<long>& idx, std::vector<M>& data, const std::vector<std::vector<M> >& datatable)
{
  idx.resize(datatable.size());
  size_t total_size = 0;
  for (size_t i = 0; i < datatable.size(); ++i) {
    const std::vector<M>& row = datatable[i];
    idx[i] = row.size();
    total_size += row.size();
  }
  data.resize(total_size);
  size_t count = 0;
  for (size_t i = 0; i < datatable.size(); ++i) {
    const std::vector<M>& row = datatable[i];
    for (size_t j = 0; j < row.size(); ++j) {
      data[count] = row[j];
      count += 1;
    }
  }
}

template <class M>
inline void split_vector(std::vector<std::vector<M> >& datatable, const std::vector<long>& idx, const std::vector<M>& data)
{
  datatable.clear();
  datatable.resize(idx.size());
  size_t count = 0;
  for (size_t i = 0; i < datatable.size(); ++i) {
    std::vector<M>& row = datatable[i];
    row.resize(idx[i]);
    for (size_t j = 0; j < row.size(); ++j) {
      row[j] = data[count];
      count += 1;
    }
  }
}

template <class M>
inline void bcast(std::vector<std::vector<M> >& datatable, const int root = 0)
{
#ifdef USE_MULTI_NODE
  long nrow, total_size;
  std::vector<long> idx;
  std::vector<M> data;
  if (get_id_node() == root) {
    concat_vector(idx, data, datatable);
    nrow = idx.size();
    total_size = data.size();
  }
  bcast(get_data(nrow), root);
  bcast(get_data(total_size), root);
  if (get_id_node() != root) {
    idx.resize(nrow);
    data.resize(total_size);
  }
  bcast(get_data(idx), root);
  bcast(get_data(data), root);
  if (get_id_node() != root) {
    split_vector(datatable, idx, data);
  }
#endif
}

inline void sync_node()
{
  long v = 1;
  glb_sum(Vector<long>(&v,1));
}

inline void display_geometry_node()
{
  TIMER("display_geo_node");
  const GeometryNode& geon = get_geometry_node();
  for (int i = 0; i < geon.num_node; ++i) {
    if (i == geon.id_node) {
      displayln(std::string(fname) + " : "
          + ssprintf("id_node = %5d ; coor_node = %s", geon.id_node, show(geon.coor_node).c_str()));
      fflush(get_output_file());
    }
    sync_node();
  }
  fflush(get_output_file());
  sync_node();
}

QLAT_END_NAMESPACE

namespace qshow {

inline std::string show(const qlat::GeometryNode& geon) {
  std::string s;
  s += ssprintf("{ initialized = %s\n", show(geon.initialized).c_str());
  s += ssprintf(", num_node    = %d\n", geon.num_node);
  s += ssprintf(", id_node     = %d\n", geon.id_node);
  s += ssprintf(", size_node   = %s\n", show(geon.size_node).c_str());
  s += ssprintf(", coor_node   = %s }", show(geon.coor_node).c_str());
  return s;
}

}

QLAT_START_NAMESPACE

using namespace qshow;

inline Coordinate plan_size_node(const int num_node)
{
  // assuming MPI is initialized ... 
  int dims[] = {0, 0, 0, 0};
  MPI_Dims_create(num_node, DIMN, dims);
  return Coordinate(dims[0], dims[1], dims[2], dims[3]);
}

inline bool is_MPI_initialized() {
  int b;
  MPI_Initialized(&b);
  return b;
}

inline int init_mpi(int* argc, char** argv[])
{
  if(!is_MPI_initialized()) MPI_Init(argc, argv);
  int num_node;
  MPI_Comm_size(MPI_COMM_WORLD, &num_node);
  displayln_info(cname() + "::begin(): " + ssprintf("MPI Initialized. NumNode = %d", num_node));
  return num_node;
}

inline void begin(const MPI_Comm& comm, const Coordinate& size_node)
  // begin Qlat with existing comm
{
  const Coordinate periods(1, 1, 1, 1);
  MPI_Cart_create(comm, DIMN, (int*)size_node.data(), (int*)periods.data(), 0, &get_comm());
  const GeometryNode& geon = get_geometry_node();
  sync_node();
  displayln_info(cname() + "::begin(): " + "MPI Cart created. GeometryNode =\n" + show(geon));
  sync_node();
  display_geometry_node();
}

inline void begin(int* argc, char** argv[], const Coordinate& size_node)
  // begin Qlat and initialize a new comm
{
  init_mpi(argc, argv);
  begin(MPI_COMM_WORLD, size_node);
}

inline void begin(int* argc, char** argv[])
  // begin Qlat and initialize a new comm with default topology
{
  int num_node = init_mpi(argc, argv);
  begin(MPI_COMM_WORLD, plan_size_node(num_node));
}

inline void end()
{
  if(is_MPI_initialized()) MPI_Finalize();
  displayln_info(cname() + "::end(): MPI Finalized.");
}

QLAT_END_NAMESPACE

