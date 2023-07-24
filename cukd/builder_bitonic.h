// ======================================================================== //
// Copyright 2019-2023 Ingo Wald                                            //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "cukd/helpers.h"
#include "cukd/box.h"

#include <cuda.h>

#include "cukd/cubit/cubit_zip.h"

namespace cukd {
  
  typedef uint32_t tag_t;

  /*! defines an abstract interface to what a 'node' in a k-d tree
    is. This needs to define the follwing:

    - node_traits::scalar_t: the scalar type of each point member (eg,
    float for a float3 node_t)

    - enum node_traits::num_dims: the number of dimensions of the
    data; e.g., a k-d tree build over float4 4d points would define
    tihs to '4'; a kd tree built over a struct htat has 3d position
    and some other additional payload would use '3'.

    - scalar_t node_traits::get(node_t &, int d) : return a
    reference to the 'd'th positional coordinate of the given node

    - enum node_traits::has_explicit_dim : whether that node type
    has a field to store an explicit split dimensoin in each
    node. If not, the k-d tree builder and traverse _have_ to use
    round-robin for split distance; otherwise, it will alwyas
    split the widest dimension

    - enum node_traits::set_dim(node_t &, int) and
    node_traits::get_dim(node_t &
  */
  template<typename node_t> struct default_node_traits {

    // ------------------------------------------------------------------
    /* part I : describes the _types_ of node of the tree, position,
       scalar, dimnensionaltiy, etc */
    // ------------------------------------------------------------------

    /*! the *logical* type used for mathematical things like distance
      computations, specifiing the location of a data point,
      etc. this defines number of dimensions, scalar type, etc, but
      leaves the node to define its own data layout */
    using point_t = node_t;

    // ------------------------------------------------------------------
    /* part II : how to extract a point or coordinate from an actual
       data struct */
    // ------------------------------------------------------------------

    /*! return a reference to the 'd'th positional coordinate of the
      given node */
    static inline __both__ const point_t &get_point(const node_t &n) { return n; }
    
    /*! return a reference to the 'd'th positional coordinate of the
      given node */
    static inline __both__
    typename scalar_type_of<point_t>::type get_coord(const node_t &n, int d)
    { return cukd::get_coord(get_point(n),d); }
    
    // ------------------------------------------------------------------
    /* part III : whether the data struct has a way of storing a split
       dimension for non-round robin paritioning, and if so, how to
       store (for building) and read (for traversing) that split
       dimensional in/from a node */
    // ------------------------------------------------------------------

    /* whether that node type has a field to store an explicit split
       dimensoin in each node. If not, the k-d tree builder and
       traverse _have_ to use round-robin for split distance;
       otherwise, it will alwyas split the widest dimensoin */
    enum { has_explicit_dim = false };
    
    /*! !{ just defining this for completeness, get/set_dim should never
      get called for this type becaues we have set has_explicit_dim
      set to false. note traversal should ONLY ever call this
      function for node_t's that define has_explicit_dim to true */
    static inline __device__ int  get_dim(const node_t &) { return -1; }
    static inline __device__ void set_dim(node_t &, int) {}
    /*! @} */
  };


  /*! defines default node traits for our own vec_float<N> vector type */
  template<int N> struct default_node_traits<vec_float<N>> {
    using node_t   = vec_float<N>;
    using scalar_t = float;
    using point_t  = node_t;
    
    enum { has_explicit_dim = false };
    
    static inline __both__ const point_t &get_point(const node_t &n) { return n; }
    static inline __both__ scalar_t get_coord(const node_t &n, int d) { return n.v[d]; }
    static inline __both__ int  get_dim(const node_t &n) { return -1; }
    static inline __both__ void set_dim(node_t &n, int dim) {}
  };
  

  
  
  // ==================================================================
  // INTERFACE SECTION
  // ==================================================================

  /*! builds a regular, "round-robin" style k-d tree over the given
    (device-side) array of points. Round-robin in this context means
    that the first dimension is sorted along x, the second along y,
    etc, going through all dimensions x->y->z... in round-robin
    fashion. point_t can be any arbitrary struct, and is assumed to
    have at least 'numDims' coordinates of type 'scalar_t', plus
    whatever other payload data is desired.

    Example 1: To build a 2D k-dtree over a CUDA int2 type (no other
    payload than the two coordinates):

    buildKDTree<int2>(....);

    Example 2: to build a 1D kd-tree over a data type of float4,
    where the first coordinate of each point is the dimension we
    want to build the kd-tree over, and the other three coordinate
    are arbitrary other payload data:

    buildKDTree<float4>(...);
  */
  template<typename node_t,
           typename node_traits=default_node_traits<node_t>>
  void buildTree(node_t *d_points,
                 int numPoints,
                 cudaStream_t stream = 0);

  /*! build a k-d over given set of points, but can build both
      round-robin-style and "generalized" k-d trees where the split
      dimension for each subtree is chosen based on the dimension
      where that subtree's domain is widest. If the
      node_traits::has_explicit_dim field is true, the latter type of
      k-d tree is build; if it is false, this function build a regular
      round-robin k-d tree instead
 */
  template<typename node_t,
           typename node_traits=default_node_traits<node_t>>
  void buildTree(node_t      *points,
                 int          numPoints,
                 box_t<typename node_traits::point_t> *worldBounds,
                 cudaStream_t stream = 0);

  template<typename node_t,
           typename node_traits=default_node_traits<node_t>>
  void computeBounds(cukd::box_t<typename node_traits::point_t> *d_bounds,
                     const node_t *d_points,
                     int numPoints,
                     cudaStream_t stream=0);

  // ==================================================================
  // IMPLEMENTATION SECTION
  // ==================================================================

  template<typename node_t, typename node_traits>
  __global__
  void computeBounds_copyFirst(box_t<typename node_traits::point_t> *d_bounds,
                               const node_t *d_points)
  {
    if (threadIdx.x != 0) return;
    
    using point_t = typename node_traits::point_t;
    const point_t point = node_traits::get_point(d_points[0]);
    d_bounds->lower = d_bounds->upper = point;
  }

  inline __device__
  float atomicMin(float *addr, float value)
  {
    float old = *addr, assumed;
    if(old <= value) return old;
    do {
      assumed = old;
      old = __int_as_float(atomicCAS((unsigned int*)addr, __float_as_int(assumed), __float_as_int(value)));
      value = min(value,old);
    } while(old!=assumed);
    return old;
  }

  inline __device__
  float atomicMax(float *addr, float value)
  {
    float old = *addr, assumed;
    if(old >= value) return old;
    do {
      assumed = old;
      old = __int_as_float(atomicCAS((unsigned int*)addr, __float_as_int(assumed), __float_as_int(value)));
      value = max(value,old);
    } while(old!=assumed);
    return old;
  }

  template<typename node_t,
           typename node_traits>
  __global__
  void computeBounds_atomicGrow(box_t<typename node_traits::point_t> *d_bounds,
                                const node_t *d_points,
                                int numPoints)
  {
    using point_t = typename node_traits::point_t;
    enum { num_dims = num_dims_of<point_t>::value };
    
    const int tid = threadIdx.x+blockIdx.x*blockDim.x;
    if (tid >= numPoints) return;
    
    using point_t = typename node_traits::point_t;
    point_t point = node_traits::get_point(d_points[tid]);
#pragma unroll(num_dims)
    for (int d=0;d<num_dims;d++) {
      float &lo = get_coord(d_bounds->lower,d);
      float &hi = get_coord(d_bounds->upper,d);
      float f = get_coord(point,d);
      atomicMin(&lo,f);
      atomicMax(&hi,f);
    }
  }

  template<typename node_t, typename node_traits>
  void computeBounds(box_t<typename node_traits::point_t> *d_bounds,
                     const node_t *d_points,
                     int numPoints,
                     cudaStream_t s)
  {
    computeBounds_copyFirst<node_t,node_traits>
      <<<1,1,0,s>>>
      (d_bounds,d_points);
    computeBounds_atomicGrow<node_t,node_traits>
      <<<divRoundUp(numPoints,128),128,0,s>>>
      (d_bounds,d_points,numPoints);
  }

  template<typename node_t, typename node_traits>
  struct ZipLess {
    inline __device__
    bool operator()(const cubit::tuple<tag_t, node_t> &a,
                    const cubit::tuple<tag_t, node_t> &b) const;
    int dim;
    // tag_t  *const __restrict__ tags;
    // node_t *const __restrict__ nodes;
    // tuple<tag_t,node_t> get(int where) const
    // { return { tags[where], nodes[where] }; }
    // void set(int where, const tuple<tag_t,node_t> &what) const
    // { tags[where] = what.u; nodes[where] = what.v; }
  };
  
  // template<typename node_t, typename node_traits>
  // struct ZipCompare {
  //   inline __device__ bool operator()
  //   (const thrust::tuple<tag_t, node_t> &a,
  //    const thrust::tuple<tag_t, node_t> &b);
  //   int dim;
  // };

  // template<typename node_t, typename node_traits>
  // struct ZipCompare {
  //   ZipCompare(const int dim, const node_t *nodes) : dim(dim), nodes(nodes) {}

  //   /*! the actual comparison operator; will perform a
  //     'zip'-comparison in that the first element is the major sort
  //     order, and the second the minor one (for those of same major
  //     sort key) */
  //   inline __device__ bool operator()
  //   (const thrust::tuple<tag_t, node_t> &a,
  //    const thrust::tuple<tag_t, node_t> &b);

    
  //   const int     dim;
  //   const node_t *nodes;
  // };

  template<typename node_t,typename node_traits>
  __global__
  void chooseInitialDim(const box_t<typename node_traits::point_t> *d_bounds,
                        node_t *d_nodes,
                        int numPoints)
  {
    using point_t  = typename node_traits::point_t;
    using scalar_t = typename scalar_type_of<point_t>::type;
    enum { num_dims = num_dims_of<point_t>::value };

    const int tid = threadIdx.x+blockIdx.x*blockDim.x;
    if (tid >= numPoints) return;
    
    int dim = arg_max(d_bounds->size());
    node_traits::set_dim(d_nodes[tid],dim);
  }
  
  /* performs the L-th step's tag update: each input tag refers to a
     subtree ID on level L, and - assuming all points and tags are in
     the expected sort order described inthe paper - this kernel will
     update each of these tags to either left or right child (or root
     node) of given subtree*/
  __global__
  void updateTag(/*! array of tags we need to update */
                 tag_t *tag,
                 /*! num elements in the tag[] array */
                 int numPoints,
                 /*! which step we're in             */
                 int L)
  {
    const int gid = threadIdx.x+blockIdx.x*blockDim.x;
    if (gid >= numPoints) return;

    const int numSettled = FullBinaryTreeOf(L).numNodes();
    if (gid < numSettled) return;

    // get the subtree that the given node is in - which is exactly
    // what the tag stores...
    int subtree = tag[gid];

    // computed the expected positoin of the pivot element for the
    // given subtree when using our speific array layout.
    const int pivotPos = ArrayLayoutInStep(L,numPoints).pivotPosOf(subtree);

    if (gid < pivotPos)
      // point is to left of pivot -> must be smaller or equal to
      // pivot in given dim -> must go to left subtree
      subtree = BinaryTree::leftChildOf(subtree);
    else if (gid > pivotPos)
      // point is to left of pivot -> must be bigger or equal to pivot
      // in given dim -> must go to right subtree
      subtree = BinaryTree::rightChildOf(subtree);
    else
      // point is _on_ the pivot position -> it's the root of that
      // subtree, don't change it.
      ;
    tag[gid] = subtree;
  }


  template<typename T> struct is_float2 { enum { value = false }; };
  template<> struct is_float2<float2> { enum { value = true }; };
    

  template<typename node_t,typename node_traits>
  inline __device__
  box_t<typename node_traits::point_t>
  findBounds(int subtree,
             const box_t<typename node_traits::point_t> *d_bounds,
             node_t *d_nodes)
  {
    using point_t  = typename node_traits::point_t;
    using scalar_t = typename scalar_type_of<point_t>::type;
    enum { num_dims = num_dims_of<point_t>::value };
    
    box_t<point_t> bounds = *d_bounds;
    int curr = subtree;
    const bool dbg = false;
    while (curr > 0) {
      const int     parent = (curr+1)/2-1;
      const node_t &parent_node = d_nodes[parent];
      const int     parent_dim
        = node_traits::has_explicit_dim
        ? node_traits::get_dim(parent_node)
        : (BinaryTree::levelOf(parent) % num_dims);
      const scalar_t parent_split_pos
        = node_traits::get_coord(parent_node,parent_dim);
      
      if (dbg) printf("# curr %i parent %i parent_dim %i parent_split %f\n",
                      curr,parent,parent_dim,parent_split_pos);

 
      if (dbg)
        printf("  > in  (%f %f)(%f %f)\n",
               ((float2&)bounds.lower).x,
               ((float2&)bounds.lower).y,
               ((float2&)bounds.upper).x,
               ((float2&)bounds.upper).y);
      if (curr & 1) {
        // curr is left child, set upper
        get_coord(bounds.upper,parent_dim)
          = min(parent_split_pos,
                get_coord(bounds.upper,parent_dim));
      } else {
        // curr is right child, set lower
        get_coord(bounds.lower,parent_dim)
          = max(parent_split_pos,
                get_coord(bounds.lower,parent_dim));
      }
      if (dbg) printf("  > out (%f %f)(%f %f)\n",
                      ((float2&)bounds.lower).x,
                      ((float2&)bounds.lower).y,
                      ((float2&)bounds.upper).x,
                      ((float2&)bounds.upper).y);
      curr = parent;
    }
    
    return bounds;
  }
  

  /* performs the L-th step's tag update: each input tag refers to a
     subtree ID on level L, and - assuming all points and tags are in
     the expected sort order described inthe paper - this kernel will
     update each of these tags to either left or right child (or root
     node) of given subtree*/
  template<typename node_t, typename node_traits>
  __global__
  void updateTagsAndSetDims(/*! array of tags we need to update */
                            const box_t<typename node_traits::point_t> *d_bounds,
                            tag_t  *tag,
                            node_t *d_nodes,
                            /*! num elements in the tag[] array */
                            int numPoints,
                            /*! which step we're in             */
                            int L)
  {
    const int gid = threadIdx.x+blockIdx.x*blockDim.x;
    if (gid >= numPoints) return;

    const int numSettled = FullBinaryTreeOf(L).numNodes();
    if (gid < numSettled) return;

    // get the subtree that the given node is in - which is exactly
    // what the tag stores...
    int subtree = tag[gid];
    box_t<typename node_traits::point_t> bounds
      = findBounds<node_t,node_traits>(subtree,d_bounds,d_nodes);
    // computed the expected positoin of the pivot element for the
    // given subtree when using our speific array layout.
    const int pivotPos = ArrayLayoutInStep(L,numPoints).pivotPosOf(subtree);

    const int   pivotDim   = node_traits::get_dim(d_nodes[pivotPos]);
    const float pivotCoord = node_traits::get_coord(d_nodes[pivotPos],pivotDim);
    
    if (gid < pivotPos) {
      // point is to left of pivot -> must be smaller or equal to
      // pivot in given dim -> must go to left subtree
      subtree = BinaryTree::leftChildOf(subtree);
      get_coord(bounds.upper,pivotDim) = pivotCoord;
    } else if (gid > pivotPos) {
      // point is to left of pivot -> must be bigger or equal to pivot
      // in given dim -> must go to right subtree
      subtree = BinaryTree::rightChildOf(subtree);
      get_coord(bounds.lower,pivotDim) = pivotCoord;
    } else
      // point is _on_ the pivot position -> it's the root of that
      // subtree, don't change it.
      ;
    if (gid != pivotPos)
      node_traits::set_dim(d_nodes[gid],arg_max(bounds.size()));
    tag[gid] = subtree;
  }
  

  template<typename node_t, typename node_traits>
  void buildTree(node_t *d_points,
                 int numPoints,
                 box_t<typename node_traits::point_t> *worldBounds,
                 cudaStream_t stream)
  {
    using point_t  = typename node_traits::point_t;
    using scalar_t = typename scalar_type_of<point_t>::type;
    enum { num_dims = num_dims_of<point_t>::value };
    
    /* thrust helper typedefs for the zip iterator, to make the code
       below more readable */
    // typedef typename thrust::device_vector<tag_t>::iterator tag_iterator;
    // typedef typename thrust::device_vector<node_t>::iterator point_iterator;
    // typedef thrust::tuple<tag_iterator,point_iterator> iterator_tuple;
    // typedef thrust::zip_iterator<iterator_tuple> tag_point_iterator;

    // check for invalid input, and return gracefully if so
    if (numPoints < 1) return;

    /* the helper array  we use to store each node's subtree ID in */
    // TODO allocate in stream?
    tag_t *tags = 0;
    CUKD_CUDA_CALL(MallocAsync((void**)&tags,numPoints*sizeof(tag_t),stream));
    CUKD_CUDA_CALL(MemsetAsync(tags,0,numPoints*sizeof(tag_t),stream));
    
    /* compute number of levels in the tree, which dicates how many
       construction steps we need to run */
    const int numLevels = BinaryTree::numLevelsFor(numPoints);
    const int deepestLevel = numLevels-1;

    if (worldBounds) 
      computeBounds<node_t,node_traits>(worldBounds,d_points,numPoints,stream);
    
    const int blockSize = 128;
    if (node_traits::has_explicit_dim) {
      if (!worldBounds)
        throw std::runtime_error
          ("cukd::builder_atomic: asked to build k-d tree over "
           "nodes with explciit dims, but no memory for world bounds provided");
      chooseInitialDim<node_t,node_traits>
        <<<divRoundUp(numPoints,blockSize),blockSize,0,stream>>>
        (worldBounds,d_points,numPoints);
      // CUKD_CUDA_CALL(StreamSynchronize(stream));
    }
    
    
    enum { zip_block_size = ((sizeof(node_t)>16)?256:1024) };
    /* now build each level, one after another, cycling through the
       dimensions */
    for (int level=0;level<deepestLevel;level++) {
      cubit::zip_sort<tag_t,node_t,ZipLess<node_t,node_traits>,zip_block_size>
        (tags,d_points,numPoints,ZipLess<node_t,node_traits>{level%num_dims});
      const int blockSize = 128;
      if (node_traits::has_explicit_dim) {
        updateTagsAndSetDims<node_t,node_traits>
          <<<divRoundUp(numPoints,blockSize),blockSize,0,stream>>>
          (worldBounds,tags,d_points,numPoints,level);
      } else {
        updateTag<<<divRoundUp(numPoints,blockSize),blockSize,0,stream>>>
          (tags,numPoints,level);
      }
      // CUKD_CUDA_CALL(StreamSynchronize(stream));
    }
    
    /* do one final sort, to put all elements in order - by now every
       element has its final (and unique) nodeID stored in the tag[]
       array, so the dimension we're sorting in really won't matter
       any more */
    cubit::zip_sort<tag_t,node_t,ZipLess<node_t,node_traits>,zip_block_size>
      (tags,d_points,numPoints,ZipLess<node_t,node_traits>{deepestLevel%num_dims});
    
    cudaFreeAsync(tags,stream);
  }







  template<typename node_t, typename node_traits>
  void buildTree(node_t *d_points,
                 int numPoints,
                 cudaStream_t stream)
  {
    using point_t  = typename node_traits::point_t;
    using scalar_t = typename scalar_type_of<point_t>::type;
    enum { num_dims = num_dims_of<point_t>::value };
    
    if (numPoints < 1) return;

    using box_t = cukd::box_t<point_t>;
    box_t *worldBounds = 0;
    if (node_traits::has_explicit_dim) {
      cudaMallocAsync((void **)&worldBounds,sizeof(*worldBounds),stream);
      computeBounds<node_t,node_traits>(worldBounds,d_points,numPoints,stream);
      
      const int blockSize = 128;
      chooseInitialDim<node_t,node_traits>
        <<<divRoundUp(numPoints,blockSize),blockSize,0,stream>>>
        (worldBounds,d_points,numPoints);
      cudaStreamSynchronize(stream);
    }
    buildTree<node_t,node_traits>(d_points,numPoints,worldBounds,stream);
    if (node_traits::has_explicit_dim) 
      cudaFreeAsync(worldBounds,stream);
  }

  

  /*! the actual comparison operator; will perform a
    'zip'-comparison in that the first element is the major sort
    order, and the second the minor one (for those of same major
    sort key) */
  template<typename node_t, typename node_traits>
  inline __device__
  bool ZipLess<node_t,node_traits>::operator()
    (const cubit::tuple<tag_t, node_t> &a,
     const cubit::tuple<tag_t, node_t> &b) const
  {
    const auto tag_a = a.u;
    const auto tag_b = b.u;

    if (tag_a < tag_b) return true;
    if (tag_a > tag_b) return false;
    
    const auto &pnt_a = a.v;
    const auto &pnt_b = b.v;
    int dim
      = node_traits::has_explicit_dim
      ? node_traits::get_dim(pnt_a)
      : this->dim;
    const auto coord_a = node_traits::get_coord(pnt_a,dim);
    const auto coord_b = node_traits::get_coord(pnt_b,dim);
    return coord_a < coord_b;
  }

}
