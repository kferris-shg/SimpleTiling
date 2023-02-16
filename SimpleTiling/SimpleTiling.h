#pragma oncex	

#include <stdint.h>
#include <intrin.h>
#include <atomic>

// SimpleTiling assumes AVX256 support
#define NUM_VECTOR_LANES 8
#define v_access(v) v.m256_f32

namespace simple_tiling_utils
{
	static constexpr uint32_t max_tiles = 64;

	// Batched colors for output - would prefer to vectorize these but the logic is awkward and I'm not sure if unsigned integer mode is possible in AVX2
	struct color_batch
	{
		uint32_t colors8bpc[NUM_VECTOR_LANES] = {}; // Assumes calculations use one pixel/lane
	};

	// Wrapper function definitions, allowing us to pass arbitrary update/draw items through a shared interface
	using draw_job = void(*)(__m256, uint32_t, color_batch*); // Draw-jobs take worker indices as well as pixel/output colors, so they can access resources created by
															  // users and not just ones internal to SimpleTiling
	using update_job = void(*)(uint32_t); // Update jobs need access to worker indices, but nothing otherwise (the actual logic is treated like an arbitrary black-box)

	// Incidental duplication here - draw and update job wrappers take worker indices as well, since they're needed for tile management
	// (checking if threads are still running, etc.)
	using draw_job_wrapper = void(*)(uint32_t, draw_job);
	using update_job_wrapper = void(*)(uint32_t, update_job);

	// Types of job (draw/update/graph), to help with work submission & processing
	enum WORK_TYPES
	{
		DRAW_WORK,
		UPDATE_WORK
	};

	enum TASK_SYNC_TYPE
	{
		EXPLICIT_SYNC, // A job in the queue has a many-to-many relation to the previous job, so the task has to wait on all instances from any previous tasks before it can launch
		IMPLICIT_SYNC // A job in the queue has a one-to-one relation to the previous job; that is, each instance only needs to depend on its immediate predecessor in the thread running the graph
					  // These jobs are implicitly synchronised by the sequencing of the queue, so no actual wait is necessary
	};

	// Frame abstraction; dispatched continuously early in program startup, independently of the main program loop
	// Frames are expected to contain update/draw/task-graph submissions (sending these directly in the main program loop is possible, but it will cause blocking on backbuffer updates)
	// Core implementation (backing thread &c) doesn't need to be user-visible, so it's declared/defined in SimpleTiling.cpp
	using frame_task = void(*)(); // Frames take no arguments and return nothing - they're empty containers for the work expected in the main program loop

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
	public:
		// Draw work resolves to an array of colors and interacts with the swap-chain
		// Work items must accept a vector of pixel indices to operate on + a pointer to a vector of 8bpc colors storing results for each pixel
		// Tile mask assumes users never request more than 64 threads/tiles
		// Draw and update work should be submitted from the main loop; whether before or after [swap_tile_buffers] is up to the user
		static void submit_draw_work(simple_tiling_utils::draw_job work, simple_tiling_utils::TASK_SYNC_TYPE sync_mode = simple_tiling_utils::IMPLICIT_SYNC, uint64_t tile_mask = 0xffffffffffffffff);

		// Update work takes a tile index, but nothing else - all other job inputs/outputs are expected to come from client statics/globals/captures
		static void submit_update_work(simple_tiling_utils::update_job work, simple_tiling_utils::TASK_SYNC_TYPE sync_mode = simple_tiling_utils::IMPLICIT_SYNC, uint64_t tile_mask = 0xffffffffffffffff);

		// Get the total number of tiles used for the current project + the number per-axis
		// Useful for managing work distribution between jobs, especially in compute work (where each tile has to manage many individual work items & not a single block of 4/8 vector lanes) (>= 4-8)
		static uint32_t GetNumTilesTotal();
		static uint32_t GetNumTilesX();
		static uint32_t GetNumTilesY();

		// Setup/shutdown
		static void setup(uint32_t num_tiles, uint32_t window_width, uint32_t window_height, bool using_interlacing);
		static void shutdown();

		// Called from the WM_PAINT block of your message pump
		static void win_paint(void* hdc, uint32_t frame_budget_ms);
};