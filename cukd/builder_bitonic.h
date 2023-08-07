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

#include "cukd/builder_common.h"
/* cuda bitonic sort package */
#include "cukd/cubit/cubit_zip.h"

namespace cukd {

  // ==================================================================
  // INTERFACE SECTION
  // ==================================================================

  /*! Builds a left-balanced k-d tree over the given data points,
    using data_traits to describe the type of data points that this
    tree is being built over (i.e., how to separate a data item's
    positional coordinates from any potential payload (if such exists,
    e.g., in a 'photon' in photon mapping), what vector/point type to
    use for this coordinate data (e.g., float3), whether the data have
    a field to store an explicit split dimensional (for Bentley and
    Samet's 'optimized' trees, etc.

    Since a (point-)k-d tree's tree topology is implicit in the
    ordering of its data points this will re-arrange the data points
    to fulfill the balanced k-d tree criterion - ie, this WILL modify
    the data array: no individual entry will get changed, but their
    order might. If data_traits::has_explcit_dims is defined this
    builder will choose each node's split dimension based on the
    widest dimension of that node's subtree's domain; if not, it will
    chose the dimension in a round-robin style, where the root level
    is split along the 'x' coordinate, the next level in y, etc

    'worldBounds' is a pointer to device-writeable memory to store the
    world-space bounding box of the data points that the builder will
    compute. If data_traits::has_explicit_dims is true this memory
    _has_ to be provided to the builder, and the builder will fill it
    in; if data_traits::has_explicit_dims is false, this memory region
    is optional: the builder _will_ fill it in if provided, but will
    ignore it if isn't.

    *** Example 1: To build a 2D k-dtree over a CUDA int2 type (no other
    payload than the two coordinates):
      
    buildTree<int2>(....);

    In this case no data_traits need to be supplied beause these will
    be auto-computed for simple cuda vector types.
      
    *** Example 2: to build a 1D kd-tree over a data type of float4,
    where the first coordinate of each point is the dimension we
    want to build the kd-tree over, and the other three coordinate
    are arbitrary other payload data:
      
    struct float2_plus_payload_traits {
    using point_t = float2;
    static inline __both__ const point_t &get_point(const float4 &n) 
    { return make_float2(n.z,n.w); }
    }
    buildTree<float4,float2_plus_payload_traits>(...);
      
    *** Example 3: assuming you have a data type 'Photon' and a
    Photon_traits has Photon_traits::has_explciit_dim defined:
      
    cukd::box_t<float3> *d_worldBounds = <cudaMalloc>;
    buildTree<Photon,Photon_traits>(..., worldBounds, ...);
      
  */
  template<typename data_t,
           typename data_traits=default_data_traits<data_t>>
  void buildTree_bitonic(data_t *d_points,
                         int numPoints,
                         cudaStream_t stream = 0);

  /*! build a k-d over given set of points, but can build both
    round-robin-style and "generalized" k-d trees where the split
    dimension for each subtree is chosen based on the dimension
    where that subtree's domain is widest. If the
    data_traits::has_explicit_dim field is true, the latter type of
    k-d tree is build; if it is false, this function build a regular
    round-robin k-d tree instead
  */
  template<typename data_t,
           typename data_traits=default_data_traits<data_t>>
  void buildTree_bitonic(data_t      *points,
                         int          numPoints,
                         box_t<typename data_traits::point_t> *worldBounds,
                         cudaStream_t stream = 0);

  // ==================================================================
  // IMPLEMENTATION SECTION
  // ==================================================================
  namespace bitonicSortBuilder {
    
    template<typename data_t, typename data_traits>
    struct ZipLess {
      inline __device__
      bool operator()(const cubit::tuple<uint32_t, data_t> &a,
                      const cubit::tuple<uint32_t, data_t> &b) const;
      int dim;
    };
  
    template<typename data_t,typename data_traits>
    __global__
    void chooseInitialDim(const box_t<typename data_traits::point_t> *d_bounds,
                          data_t *d_nodes,
                          int numPoints)
    {
      using point_t  = typename data_traits::point_t;
      using scalar_t = typename scalar_type_of<point_t>::type;
      enum { num_dims = num_dims_of<point_t>::value };

      const int tid = threadIdx.x+blockIdx.x*blockDim.x;
      if (tid >= numPoints) return;
    
      int dim = arg_max(d_bounds->size());
      data_traits::set_dim(d_nodes[tid],dim);
    }
  
    /* performs the L-th step's tag update: each input tag refers to a
       subtree ID on level L, and - assuming all points and tags are in
       the expected sort order described inthe paper - this kernel will
       update each of these tags to either left or right child (or root
       node) of given subtree*/
    __global__
    void updateTag(/*! array of tags we need to update */
                   uint32_t *tag,
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


    /* performs the L-th step's tag update: each input tag refers to a
       subtree ID on level L, and - assuming all points and tags are in
       the expected sort order described inthe paper - this kernel will
       update each of these tags to either left or right child (or root
       node) of given subtree*/
    template<typename data_t, typename data_traits>
    __global__
    void updateTagsAndSetDims(/*! array of tags we need to update */
                              const box_t<typename data_traits::point_t> *d_bounds,
                              uint32_t  *tag,
                              data_t *d_nodes,
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
      box_t<typename data_traits::point_t> bounds
        = findBounds<data_t,data_traits>(subtree,d_bounds,d_nodes);
      // computed the expected positoin of the pivot element for the
      // given subtree when using our speific array layout.
      const int pivotPos = ArrayLayoutInStep(L,numPoints).pivotPosOf(subtree);

      const int   pivotDim   = data_traits::get_dim(d_nodes[pivotPos]);
      const float pivotCoord = data_traits::get_coord(d_nodes[pivotPos],pivotDim);
    
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
        data_traits::set_dim(d_nodes[gid],arg_max(bounds.size()));
      tag[gid] = subtree;
    }
  

    template<typename data_t, typename data_traits>
    void buildTree(data_t *d_points,
                   int numPoints,
                   box_t<typename data_traits::point_t> *worldBounds,
                   cudaStream_t stream)
    {
      using point_t  = typename data_traits::point_t;
      using scalar_t = typename scalar_type_of<point_t>::type;
      enum { num_dims = num_dims_of<point_t>::value };
    
      /* thrust helper typedefs for the zip iterator, to make the code
         below more readable */
      // typedef typename thrust::device_vector<uint32_t>::iterator tag_iterator;
      // typedef typename thrust::device_vector<data_t>::iterator point_iterator;
      // typedef thrust::tuple<tag_iterator,point_iterator> iterator_tuple;
      // typedef thrust::zip_iterator<iterator_tuple> tag_point_iterator;

      // check for invalid input, and return gracefully if so
      if (numPoints < 1) return;

      /* the helper array  we use to store each node's subtree ID in */
      // TODO allocate in stream?
      uint32_t *tags = 0;
      CUKD_CUDA_CALL(MallocAsync((void**)&tags,numPoints*sizeof(uint32_t),stream));
      CUKD_CUDA_CALL(MemsetAsync(tags,0,numPoints*sizeof(uint32_t),stream));
    
      /* compute number of levels in the tree, which dicates how many
         construction steps we need to run */
      const int numLevels = BinaryTree::numLevelsFor(numPoints);
      const int deepestLevel = numLevels-1;

      if (worldBounds) 
        computeBounds<data_t,data_traits>(worldBounds,d_points,numPoints,stream);
    
      const int blockSize = 128;
      if (data_traits::has_explicit_dim) {
        if (!worldBounds)
          throw std::runtime_error
            ("cukd::builder_atomic: asked to build k-d tree over "
             "nodes with explciit dims, but no memory for world bounds provided");
        chooseInitialDim<data_t,data_traits>
          <<<divRoundUp(numPoints,blockSize),blockSize,0,stream>>>
          (worldBounds,d_points,numPoints);
        // CUKD_CUDA_CALL(StreamSynchronize(stream));
      }
    
    
      enum { zip_block_size = ((sizeof(data_t)>16)?256:1024) };
      /* now build each level, one after another, cycling through the
         dimensions */
      for (int level=0;level<deepestLevel;level++) {
        cubit::zip_sort<uint32_t,data_t,ZipLess<data_t,data_traits>,zip_block_size>
          (tags,d_points,numPoints,ZipLess<data_t,data_traits>{level%num_dims});
        const int blockSize = 128;
        if (data_traits::has_explicit_dim) {
          updateTagsAndSetDims<data_t,data_traits>
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
      cubit::zip_sort<uint32_t,data_t,ZipLess<data_t,data_traits>,zip_block_size>
        (tags,d_points,numPoints,ZipLess<data_t,data_traits>{deepestLevel%num_dims});
    
      cudaFreeAsync(tags,stream);
    }


    /*! the actual comparison operator; will perform a
      'zip'-comparison in that the first element is the major sort
      order, and the second the minor one (for those of same major
      sort key) */
    template<typename data_t, typename data_traits>
    inline __device__
    bool ZipLess<data_t,data_traits>::operator()
      (const cubit::tuple<uint32_t, data_t> &a,
       const cubit::tuple<uint32_t, data_t> &b) const
    {
      const auto tag_a = a.u;
      const auto tag_b = b.u;

      if (tag_a < tag_b) return true;
      if (tag_a > tag_b) return false;
    
      const auto &pnt_a = a.v;
      const auto &pnt_b = b.v;
      int dim
        = data_traits::has_explicit_dim
        ? data_traits::get_dim(pnt_a)
        : this->dim;
      const auto coord_a = data_traits::get_coord(pnt_a,dim);
      const auto coord_b = data_traits::get_coord(pnt_b,dim);
      return coord_a < coord_b;
    }

  } // ::cukd::bitonicSortBuilder

  template<typename data_t, typename data_traits>
  void buildTree_bitonic(data_t *d_points,
                         int numPoints,
                         cudaStream_t stream)
  {
    using namespace bitonicSortBuilder;
    
    using point_t  = typename data_traits::point_t;
    using scalar_t = typename scalar_type_of<point_t>::type;
    enum { num_dims = num_dims_of<point_t>::value };
    
    if (numPoints < 1) return;

    using box_t = cukd::box_t<point_t>;
    box_t *worldBounds = 0;
    if (data_traits::has_explicit_dim) {
      cudaMallocAsync((void **)&worldBounds,sizeof(*worldBounds),stream);
      computeBounds<data_t,data_traits>(worldBounds,d_points,numPoints,stream);
      
      const int blockSize = 128;
      chooseInitialDim<data_t,data_traits>
        <<<divRoundUp(numPoints,blockSize),blockSize,0,stream>>>
        (worldBounds,d_points,numPoints);
      cudaStreamSynchronize(stream);
    }
    bitonicSortBuilder::buildTree<data_t,data_traits>(d_points,numPoints,worldBounds,stream);
    if (data_traits::has_explicit_dim) 
      cudaFreeAsync(worldBounds,stream);
  }

  

}