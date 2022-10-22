// SimpleTiling.cpp : Defines the functions for the static library.
//

#include "SimpleTiling.h"
#include <thread>
#include <vector>
#include <intrin.h>
#include <cassert>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Need to...
// Implement setup
// Wrap Win32 boilerplate for image copies
// Do some test renders ^_^

// Define statics declared in [SimpleTiling.h]
uint32_t canvas_width = 0;
uint32_t canvas_height = 0;
BITMAPINFO canvas_bmi;
uint32_t* back_buffer = nullptr;

// Our design goal is to automate tiling/thread scheduling, so that rendering apps can focus on their core details instead
namespace simple_tiling_utils
{
	enum WORK_TYPES
	{
		DRAW_WORK,
		UPDATE_WORK
	};

	template<WORK_TYPES work_type>
	struct job_q
	{
		// Max number of queued jobs per-thread
		static constexpr int32_t max_queued_jobs = 32;

		// Work submission (all lambdas!)
		using job_wrapper = std::conditional<work_type == DRAW_WORK, draw_job_wrapper, update_job_wrapper>::type;
		using wrapped_job = std::conditional<work_type == DRAW_WORK, draw_job, update_job>::type;
		void append_job(job_wrapper wrapper, wrapped_job job, job_wrapper_inputs wrapper_inputs)
		{
			// Drop work submitted beyond our queue limit
			//assert(front < max_queued_jobs);
			if (front < max_queued_jobs)
			{
				wrappers[front] = wrapper;
				jobs[front] = job;
				inputs[front] = wrapper_inputs;
				front++;
			}
			else
			{
#if _DEBUG
				//DebugBreak();
#endif
			}
		}

		void consume_job()
		{
			// Only consume jobs if at least one is available in the queue
			assert(front > 0);

			// Spin until a consumeable job shows up
			while (front == 0)
			{
				/* Spin */
			}

			// All good! Consume the most recently-submitted job
			wrappers[front-1](inputs[front-1], jobs[front-1]);
			front--;
		}

		// Core job queue
		job_wrapper wrappers[max_queued_jobs] = {};
		job_wrapper_inputs inputs[max_queued_jobs] = {};
		wrapped_job jobs[max_queued_jobs] = {};
		std::atomic_int front = 0;
	};
};

simple_tiling_utils::job_q<simple_tiling_utils::DRAW_WORK> tile_draw_jobs[simple_tiling_utils::max_tiles];
simple_tiling_utils::job_q<simple_tiling_utils::UPDATE_WORK> tile_update_jobs[simple_tiling_utils::max_tiles];
simple_tiling_utils::color_batch* tileBuffers[simple_tiling_utils::max_tiles] = {};

template<typename vectype>
struct fast_vec
{
	vectype* data = nullptr;
	size_t front = 0;
	void init(vectype* memory)
	{
		data = memory;
	}
	void push_back(vectype elt)
	{
		data[front] = elt;
		front++;
	}
	void clear()
	{
		front = 0;
	}
	size_t size()
	{
		return front;
	}
	vectype at(uint32_t i)
	{
		return data[i];
	}
};

simple_tiling_utils::color_batch* stagingTileBuffers[simple_tiling_utils::max_tiles] = {}; // For temporary writes while [tileBuffers] are blocked from the main thread
fast_vec<uint32_t> blockedTileStrips[simple_tiling_utils::max_tiles] = {}; // Entire rows are blocked at once, so instead of storing duplicate pixel information for
																		   // each staged color we can store a buffer of blocked strips instead - this should improve
																		   // cache performance for staging reads/writes

uint32_t tileMinX[simple_tiling_utils::max_tiles] = {};
uint32_t tileMaxX[simple_tiling_utils::max_tiles] = {};
uint32_t tileMinY[simple_tiling_utils::max_tiles] = {};
uint32_t tileMaxY[simple_tiling_utils::max_tiles] = {};
std::atomic_bool tile_running[simple_tiling_utils::max_tiles] = {};
std::atomic_bool tile_shutdown_success[simple_tiling_utils::max_tiles] = {};

std::atomic<simple_tiling_utils::TILE_STATES> tile_states[simple_tiling_utils::max_tiles] = {};
std::thread tiles[simple_tiling_utils::max_tiles];
uint32_t numTiles = 0;
uint32_t numTilesX = 0;
uint32_t numTilesY = 0;

void simple_tiling::submit_draw_work_internal(simple_tiling_utils::draw_job_wrapper work, simple_tiling_utils::job_wrapper_inputs wrapper_inputs,
											  simple_tiling_utils::draw_job wrapped_job, uint32_t tile_ndx)
{
	tile_draw_jobs[tile_ndx].append_job(work, wrapped_job, wrapper_inputs);
}

void simple_tiling::submit_update_work_internal(simple_tiling_utils::update_job_wrapper work, simple_tiling_utils::job_wrapper_inputs wrapper_inputs,
 											    simple_tiling_utils::update_job wrapped_job, uint32_t tile_ndx)
{
	tile_update_jobs[tile_ndx].append_job(work, wrapped_job, wrapper_inputs);
}

void draw_wrapper(simple_tiling_utils::job_wrapper_inputs wrapper_inputs, simple_tiling_utils::draw_job wrapped_job)
{
	const uint32_t minX = tileMinX[wrapper_inputs.data];
	const uint32_t maxX = tileMaxX[wrapper_inputs.data];
	const uint32_t minY = tileMinY[wrapper_inputs.data];
	const uint32_t maxY = tileMaxY[wrapper_inputs.data];
	bool unblocked = false;

	for (uint32_t pixel_row = minY; pixel_row < maxY; pixel_row++)
	{
		// Tile state checks - try to do these as infrequently as possible
		bool tileBlocked = false;
		if (!unblocked)
		{
			tileBlocked = (tile_states[wrapper_inputs.data] == simple_tiling_utils::UPLOADING);
			if (!tileBlocked)
			{
				unblocked = true;
				tile_states[static_cast<uint32_t>(wrapper_inputs.data)] = simple_tiling_utils::PROCESSING;
			}
		}

		if (tileBlocked)
		{
			blockedTileStrips[wrapper_inputs.data].push_back(pixel_row - minY);
		}

		//  Core pixel processing
		for (uint32_t pixel_batch = minX; pixel_batch < maxX; pixel_batch += NUM_VECTOR_LANES) // For each vectorized pixel batch
		{
			// Issue work
			simple_tiling_utils::color_batch batch_colors;
			const float init_px = static_cast<float>((pixel_row * canvas_width) + pixel_batch);
#if (NUM_VECTOR_LANES == 4)
			{
				wrapped_job(v_op(set_ps)(init_px, init_px + 1, init_px + 2, init_px + 3), &batch_colors);
			}
#elif (NUM_VECTOR_LANES == 8)
			{
				wrapped_job(v_op(set_ps)(init_px, init_px + 1, init_px + 2, init_px + 3, init_px + 4, init_px + 5, init_px + 6, init_px + 7), &batch_colors);
			}
#else
#error("unsupported vector width");
#endif

			// Not uploading, safe to copy out tile color ^_^
			const uint32_t tile_width = maxX - minX;
			const uint32_t tile_height = maxY - minY;
			const uint32_t tile_px_x = pixel_batch - minX;
			const uint32_t tile_px_y = pixel_row - minY;
			const uint32_t tile_px = (tile_px_y * tile_width) + tile_px_x;

			// Keep working through blocks, staging colors until the main tile buffer becomes available again
			simple_tiling_utils::color_batch* dstColors = tileBlocked ? stagingTileBuffers[wrapper_inputs.data] : tileBuffers[wrapper_inputs.data];
			dstColors[tile_px / NUM_VECTOR_LANES] = batch_colors;
			//memset(tileBuffers[static_cast<uint32_t>(wrapper_inputs.data)], 0xff, tile_width * tile_height * sizeof(simple_tiling_utils::v_type));
		}
	}

	// Spin here if the tile-buffer is still blocked
	if (tile_running[wrapper_inputs.data])
	{
		if (tile_states[wrapper_inputs.data] == simple_tiling_utils::UPLOADING)
		{
			tile_states[wrapper_inputs.data].wait(simple_tiling_utils::UPLOADING);
		}
	}

	// Safe to write to the tile buffer - blast out staging colors
	// No reason to execute these if tiling has been stopped anyway
	if (tile_running[wrapper_inputs.data])
	{
		const uint32_t tile_width_vectors = (maxX - minX) / NUM_VECTOR_LANES;
		const size_t num_strips = blockedTileStrips[wrapper_inputs.data].size();
		for (size_t i = 0; i < num_strips; i++)
		{
			const uint32_t pixel_row = blockedTileStrips[wrapper_inputs.data].at(i);
			const uint32_t strip_ndx = (pixel_row * tile_width_vectors);
			memcpy(tileBuffers[wrapper_inputs.data] + strip_ndx, stagingTileBuffers[wrapper_inputs.data] + strip_ndx, sizeof(simple_tiling_utils::color_batch) * tile_width_vectors);
		}
		blockedTileStrips[wrapper_inputs.data].clear();

		// Signal the main thread that this tile has finished working, so it can be safely copied to the back-buffer
		tile_states[static_cast<uint32_t>(wrapper_inputs.data)] = simple_tiling_utils::UPLOADING;
	}
}

void simple_tiling::submit_draw_work(simple_tiling_utils::draw_job work, uint64_t tile_mask)
{
	for (uint32_t i = 0; i < numTiles; i++) // For each tile
	{
		if (tile_mask & (0x1ull << i)) // Skip processing masked tiles
		{
			simple_tiling_utils::job_wrapper_inputs args;
			args.data = i;
			submit_draw_work_internal(draw_wrapper, args, work, i);
		}
		else if (tile_states[i] != simple_tiling_utils::UPLOADING)
		{
			// Not uploading or processing, so flag this tile as IDLE
			tile_states[i] = simple_tiling_utils::IDLE;
		}
	}
}

void thread_main(uint32_t tile_ndx)
{
	// Should use condi-vars here to avoid spamming all threads all the time
	while (tile_running[tile_ndx])
	{
		// Consume draw jobs, then consume update jobs
		// I don't *think* that should cause any issues
		// (no reason updates drawing during an upload would be a problem, unless the user is intentionally trying to make the main/tile threads interfere with each other)
		if (tile_draw_jobs[tile_ndx].front > 0)
		{
			tile_draw_jobs[tile_ndx].consume_job();
		}
		if (tile_update_jobs[tile_ndx].front > 0)
		{
			tile_update_jobs[tile_ndx].consume_job();
		}
	}
	tile_shutdown_success[tile_ndx] = true;
}

static constexpr uint64_t mem_budget = 100000000;
uint8_t* tiling_pool = nullptr;
uint8_t* alloc_front = tiling_pool;
template<typename t>
t* alloc()
{
	t* ptr = reinterpret_cast<t*>(alloc_front);
	alloc_front += sizeof(t);
	return ptr;
}

template<typename t>
t* alloc_array(uint64_t num_elts)
{
	t* ptr = reinterpret_cast<t*>(alloc_front);
	alloc_front += sizeof(t) * num_elts;
	return ptr;
}

// Call after your application's window setup
//uint32_t* test_canvas = nullptr;
void simple_tiling::setup(uint32_t num_tiles, uint32_t window_width, uint32_t window_height)
{
	// Resolve canvas dimensions
	canvas_width = window_width;
	canvas_height = window_height;

	// Resolve tile dimensions
	//////////////////////////

	// If numTiles is square, use square tiling
	numTiles = num_tiles;
	const double root = sqrt(static_cast<double>(num_tiles));
	if ((root - floor(root)) == 0)
	{
		numTilesX = static_cast<uint32_t>(root);
		numTilesY = static_cast<uint32_t>(root);
	}
	else if (num_tiles % 2 == 0) // If numTiles is modulo-2, make two columns of N tiles each
	{
		numTilesX = 2;
		numTilesY = num_tiles / 2;
	}
	else // If numTiles is odd and non-square, add one and do the same as above
		 // (not ideal, but easiest for worker layout)
	{
		numTiles++;
		num_tiles++;
		numTilesX = 2;
		numTilesY = num_tiles / 2;
	}

	const uint32_t tile_width_px = canvas_width / numTilesX;
	const uint32_t tile_height_px = canvas_height / numTilesY;
	uint32_t tileCtr = 0;
	for (uint32_t x = 0; x < numTilesX; x++)
	{
		for (uint32_t y = 0; y < numTilesY; y++)
		{
			tileMinX[tileCtr] = tile_width_px * x;
			tileMaxX[tileCtr] = tileMinX[tileCtr] + tile_width_px;

			tileMinY[tileCtr] = tile_height_px * y;
			tileMaxY[tileCtr] = tileMinY[tileCtr] + tile_height_px;
			tileCtr++;
		}
	}

	// Allocate working memory
	tiling_pool = (uint8_t*)malloc(mem_budget); // 100MB to start with, we can increase/decrease as we need
	alloc_front = tiling_pool;

	// Initialize tile data + thread controls
	const uint32_t tile_width_vectors = tile_width_px / NUM_VECTOR_LANES;
	const uint32_t tile_height_vectors = tile_height_px;
	const uint32_t tile_area_vectors = tile_width_vectors * tile_height_vectors;
	memset(tile_draw_jobs, 0x0, sizeof(simple_tiling_utils::job_q<simple_tiling_utils::DRAW_WORK>) * num_tiles);
	memset(tile_update_jobs, 0x0, sizeof(simple_tiling_utils::job_q<simple_tiling_utils::DRAW_WORK>) * num_tiles);
	for (uint32_t i = 0; i < num_tiles; i++)
	{
		tileBuffers[i] = alloc_array<simple_tiling_utils::color_batch>(tile_area_vectors);
		tile_running[i] = true;
		tile_shutdown_success[i] = false;
		tile_states[i] = simple_tiling_utils::IDLE;
		tiles[i] = std::thread(thread_main, i);
		stagingTileBuffers[i] = alloc_array<simple_tiling_utils::color_batch>(tile_area_vectors); // Free allocation, ew; should use custom vector that allocates with [alloc_array]
		blockedTileStrips[i].init(alloc_array<uint32_t>(tile_area_vectors));
	}

	// Allocate the canvas back-buffer
	//test_canvas = alloc_array<uint32_t>(canvas_width * canvas_height);
	back_buffer = alloc_array<uint32_t>(canvas_width * canvas_height);
	//memset(back_buffer, 0xff, canvas_width * canvas_height * 4);

	// Prepare BITMAPINFO (needed for Windows' blitting interface)
	BITMAPINFO nfo;
	ZeroMemory(&nfo, sizeof(BITMAPINFO));
	nfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	nfo.bmiHeader.biWidth = canvas_width;
	nfo.bmiHeader.biHeight = canvas_height;
	nfo.bmiHeader.biPlanes = 1;
	nfo.bmiHeader.biBitCount = 32;
	nfo.bmiHeader.biCompression = BI_RGB;
	nfo.bmiHeader.biSizeImage = 0;
	nfo.bmiHeader.biXPelsPerMeter = 0; // 3840 / 36cm
	nfo.bmiHeader.biYPelsPerMeter = 0; // 2160 / 16cm
	nfo.bmiHeader.biClrUsed = FALSE;
	nfo.bmiHeader.biClrImportant = FALSE;
	canvas_bmi = nfo;
}

void simple_tiling::shutdown()
{
	// Terminate tile threads
	for (uint32_t i = 0; i < numTiles; i++)
	{
		tile_running[i] = false;
		while (!tile_shutdown_success[i])
		{
			// Repeatedly notify blocked threads until they unblock
			tile_states[i].store(simple_tiling_utils::IDLE);
			tile_states[i].notify_one();
		}
	}

	// Separate loops so that calls to [join] are delayed enough for tile states to be well-defined
	for (uint32_t i = 0; i < numTiles; i++)
	{
		tiles[i].join();
	}

	// ... other shutdown things ... //
	free(tiling_pool); // <3 linear allocators
}

// Called from your application's main loop
void simple_tiling::swap_tile_buffers()
{
	for (uint32_t i = 0; i < numTiles; i++)
	{
		//memset(back_buffer + canvas_width * tileMinY[i], 0xff, canvas_width * sizeof(uint32_t));
		if (tile_states[i] == simple_tiling_utils::UPLOADING)
		{
			const uint32_t minY = tileMinY[i];
			const uint32_t minX = tileMinX[i];
			const uint32_t maxY = tileMaxY[i];
			const uint32_t maxX = tileMaxX[i];
			const uint32_t dest_w = (maxX - minX);
			const uint32_t src_w = (maxX - minX) / NUM_VECTOR_LANES;

			auto in_ptr = tileBuffers[i];
			auto out_ptr = back_buffer + ((minY * canvas_width) + minX);

			for (uint32_t y = minY; y < maxY; y++)
			{
				memcpy(out_ptr, in_ptr, sizeof(uint32_t) * dest_w);
				in_ptr += src_w;
				out_ptr += canvas_width;
			}

			// Assume that we want to start drawing again immediately after copying-out
			// (=> enables frames to begin as soon as possible)
			// If we don't want to draw immediately, client code can supply a mask to [submit_work(...)] to skip it in the next frame,
			// which will cause its state to swap over from [PROCESSING] to [IDLE] (the IDLE state doesn't affect anything itself, but it can be useful
			// for debugging thread occupancy or verifying if threads have been separated from rendering before taking on update work)
			tile_states[i] = simple_tiling_utils::PROCESSING;
			tile_states[i].notify_one();
		}
	}
}

// Called from the WM_PAINT block of your message pump
// (between BeginPaint() and EndPaint())
void simple_tiling::win_paint(void* hdc)
{
	uint32_t test = SetDIBitsToDevice(static_cast<HDC>(hdc), 0, 0, canvas_width, canvas_height, 0, 0, 0, canvas_height, back_buffer, &canvas_bmi, DIB_RGB_COLORS);
	assert(test);
}
