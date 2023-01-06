#pragma once

#include <stdint.h>
#include <intrin.h>

// Width-agnostic load/store functions
#define NUM_VECTOR_LANES 8 // Can be either 4 (SSE) or 8 (AVX)

#if (NUM_VECTOR_LANES == 4)
#define v_op(op) _mm_op
#elif (NUM_VECTOR_LANES == 8)
#define v_op(op) _mm256_##op
#else
#error("unsupported vector width");
#endif

#if (NUM_VECTOR_LANES == 4)
#define v_access(v) v.m
#elif (NUM_VECTOR_LANES == 8)
#define v_access(v) v.m256_f32
#else
#error("unsupported vector width");
#endif

#if (NUM_VECTOR_LANES == 4)
#define v_i __m128i
#elif (NUM_VECTOR_LANES == 8)
#define v_i __m256i
#else
#error("unsupported vector width");
#endif

namespace simple_tiling_utils
{
#if (NUM_VECTOR_LANES == 4)
	using v_type = __m128;
#elif (NUM_VECTOR_LANES == 8)
	using v_type = __m256;
#else
#error("unsupported vector width");
#endif
	static constexpr uint32_t max_tiles = 64;

	// Batched colors for output - would prefer to vectorize these but the logic is awkward and I'm not sure if unsigned integer mode is possible in AVX2
	struct color_batch
	{
		uint32_t colors8bpc[NUM_VECTOR_LANES] = {}; // Assumes calculations use one pixel/lane
	};

	// 64 bits of arbitrary data, interpreted by library code
	struct job_wrapper_inputs
	{
		uint64_t data = 0;
	};

	// Wrapper function definitions, allowing us to pass arbitrary update/draw items through a shared interface
	using draw_job = void(*)(v_type, color_batch*);
	using update_job = void(*)(uint32_t); // Update jobs need access to worker indices, but nothing otherwise (the actual logic is treated like an arbitrary black-box)
	using draw_job_wrapper = void(*)(job_wrapper_inputs, draw_job);
	using update_job_wrapper = void(*)(job_wrapper_inputs, update_job);

	// Types of job (draw/update), to help with work submission & processing
	enum WORK_TYPES
	{
		DRAW_WORK,
		UPDATE_WORK
	};

	// Encapsulated graph of tasks, to be submitted in a single block (as in a graphics pipeline)
	// These graphs should generally be prepared on start-up and dispatched in the main program loop
	// (unless something changes at runtime to regenerate them, e.g. new asset, different screen size, etc)
	struct task_graph
	{
		public:
			static constexpr uint32_t max_graph_depth = 8;
			void append_draw();
			void append_update();

		private:
			update_job packed_update_jobs[max_graph_depth] = {};
			draw_job packed_draw_jobs[max_graph_depth] = {};
			WORK_TYPES workTypes[max_graph_depth] = {};
			uint32_t job_count = 0;
	};


	// Thread signals have three separate states; IDLE, PROCESSING, and UPLOADING
	// Threads swap to UPLOADING when they're ready for copy-out, and back to IDLE when the CPU finishes with their datá
	// Threads can process work in any state, but not write out to the scratch buffer until they enter IDLE or PROCESSING
	enum TILE_STATES
	{
		IDLE,
		PROCESSING,
		UPLOADING
	};
};

class simple_tiling
{
	private:
		// Work-items supplied from [submit_draw_work] or [submit_update_work] are wrapped in a generic lambda and propagated to a job queue here
		static void submit_draw_work_internal(simple_tiling_utils::draw_job_wrapper work, simple_tiling_utils::job_wrapper_inputs wrapper_inputs,
											  simple_tiling_utils::draw_job wrapped_job, uint32_t tile_ndx);
		static void submit_update_work_internal(simple_tiling_utils::update_job_wrapper work, simple_tiling_utils::job_wrapper_inputs wrapper_inputs,
												simple_tiling_utils::update_job wrapped_job, uint32_t tile_ndx);

	public:
		// Draw work resolves to an array of colors and interacts with the swap-chain
		// Work items must accept a vector of pixel indices to operate on + a pointer to a vector of 8bpc colors storing results for each pixel
		// Tile mask assumes users never request more than 64 threads/tiles
		// Draw and update work should be submitted from the main loop; whether before or after [swap_tile_buffers] is up to the user
		static void submit_draw_work(simple_tiling_utils::draw_job work, uint64_t tile_mask = 0xffffffffffffffff);

		// Update work takes a tile index, but nothing else - all other job inputs/outputs are expected to come from client statics/globals/captures
		static void submit_update_work(simple_tiling_utils::update_job work, uint64_t tile_mask = 0xffffffffffffffff);

		// Submit a linear graph of jobs to execute; each graph can contain a mix of update & draw work
		// Update jobs with dependencies on draw work require explicit waits, since those two pipes are asynchronous on each thread;
		// still not certain how to implement that; something that doesn't increase contention or memory footprint would be ideal
		static void submit_task_graph(simple_tiling_utils::task_graph& graph);

		// Simple sync primitives - useful when the cpu needs to go wide to process work but can't progress until the work is finished
		static void WaitForTileProcessing(uint32_t tile_ndx);
		static void WaitForTileUpload(uint32_t tile_ndx);

		// Get the total number of tiles used for the current project + the number per-axis
		// Useful for managing work distribution between jobs, especially in compute work (where each tile has to manage many individual work items & not a single block of 4/8 vector lanes) (>= 4-8)
		static uint32_t GetNumTilesTotal();
		static uint32_t GetNumTilesX();
		static uint32_t GetNumTilesY();

		// Setup/shutdown
		static void setup(uint32_t num_tiles, uint32_t window_width, uint32_t window_height, bool using_interlacing);
		static void shutdown();

		// Called from the WM_PAINT block of your message pump
		static void win_paint(void* hdc);
};