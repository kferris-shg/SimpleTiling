// SimpleTiling.cpp : Defines the functions for the static library.
//

#include "SimpleTiling.h"
#include <thread>
#include <vector>
#include <intrin.h>
#include <cassert>
#include "..\ThirdParty\tracy-0.8\Tracy.hpp"

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

// Padded wrapper used with variables intended for specific threads, to avoid false sharing
struct XThreadWrapper
{
	enum BLIT_MESSAGING
	{
		AWAITING_COPY,
		COPIED
	};

	struct data
	{
		uint32_t tileMinX = 0;
		uint32_t tileMaxX = 0;
		uint32_t tileMinY = 0;
		uint32_t tileMaxY = 0;
		uint8_t interlace_offset_x = 0; // 2D interlacing on every second row/column, offset is either 0 or 1; mostly only used for draw jobs
		uint8_t interlace_offset_y = 0; // 2D interlacing on every second row/column, offset is either 0 or 1; mostly only used for draw jobs
		std::atomic_bool tile_running = {};
		std::atomic_bool tile_shutdown_success = {};
		std::atomic<simple_tiling_utils::TILE_STATES> tile_state = {};
		std::atomic<BLIT_MESSAGING> blit_state = {};
		std::thread tile;
	};
	data threadData = {};

	static constexpr uint8_t padlen = sizeof(data) > 64 ? 0 : (64 - sizeof(data));
	uint8_t padding[padlen] = {};

	XThreadWrapper(data inputs)
	{
		memcpy(&threadData, &inputs, sizeof(data));
		memset(&padding, 0x0, sizeof(padding));
	}
	XThreadWrapper() {}
};

XThreadWrapper tile_data[simple_tiling_utils::max_tiles] = {};

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

bool interlacing = true;
void draw_wrapper(simple_tiling_utils::job_wrapper_inputs wrapper_inputs, simple_tiling_utils::draw_job wrapped_job)
{
	ZoneScoped;
	XThreadWrapper::data& tileInfo = tile_data[wrapper_inputs.data].threadData;
	const uint32_t minX = tileInfo.tileMinX;
	const uint32_t maxX = tileInfo.tileMaxX;
	const uint32_t minY = tileInfo.tileMinY;
	const uint32_t maxY = tileInfo.tileMaxY;
	const uint8_t interlace_offs_x = tileInfo.interlace_offset_x;
	const uint8_t interlace_offs_y = tileInfo.interlace_offset_y;
	bool unblocked = false;

	// Need to de-interlace X and Y separately - otherwise one axis will always have gaps
	uint32_t dy = interlace_offs_y * interlacing;
	uint32_t dx = interlace_offs_x * NUM_VECTOR_LANES * interlacing;
	for (uint32_t pixel_row = minY + dy; pixel_row < maxY; pixel_row += (1 + dy))
	{
		//  Core pixel processing
		for (uint32_t pixel_batch = minX + dx; pixel_batch < maxX; pixel_batch += (NUM_VECTOR_LANES + dx)) // For each vectorized pixel batch
		{
			// Define outputs
			const uint32_t tile_width = maxX - minX;
			const uint32_t tile_height = maxY - minY;
			const uint32_t tile_px_x = pixel_batch - minX;
			const uint32_t tile_px_y = pixel_row - minY;
			const uint32_t tile_px = (tile_px_y * tile_width) + tile_px_x;
			simple_tiling_utils::color_batch* batch_colors = tileBuffers[wrapper_inputs.data] + (tile_px / NUM_VECTOR_LANES);

			// Issue work
			const float init_px = static_cast<float>((pixel_row * canvas_width) + pixel_batch);
#if (NUM_VECTOR_LANES == 4)
			{
				wrapped_job(v_op(set_ps)(init_px, init_px + 1, init_px + 2, init_px + 3), &batch_colors);
			}
#elif (NUM_VECTOR_LANES == 8)
			{
				wrapped_job(v_op(set_ps)(init_px, init_px + 1, init_px + 2, init_px + 3, init_px + 4, init_px + 5, init_px + 6, init_px + 7), batch_colors);
			}
#else
#error("unsupported vector width");
#endif
		}
	}

	// No reason to execute copy-outs if tiling has been stopped anyway
	if (tileInfo.tile_running)
	{
		// Signal the main thread that this tile is uploading, so the main thread knows to wait for it
		tileInfo.tile_state = simple_tiling_utils::UPLOADING;

		// Run back-buffer copies on source threads to prevent them stumbling over each other
		// Only run copies if the main thread is currently waiting on one
		if (tileInfo.blit_state == XThreadWrapper::AWAITING_COPY)
		{
			const uint32_t dest_w = (maxX - minX);
			const uint32_t src_w = (maxX - minX) / NUM_VECTOR_LANES;

			auto in_ptr = tileBuffers[wrapper_inputs.data];
			auto out_ptr = back_buffer + ((minY * canvas_width) + minX);

			for (uint32_t y = minY; y < maxY; y++)
			{
				memcpy(out_ptr, in_ptr, sizeof(uint32_t) * dest_w);
				in_ptr += src_w;
				out_ptr += canvas_width;
			}

			tileInfo.blit_state = XThreadWrapper::COPIED;

			// Signal the main thread that we've finished copying-out this tile, so the main thread can proceed with e.g. frame blits
			tileInfo.tile_state = simple_tiling_utils::PROCESSING;
			tileInfo.tile_state.notify_one();
		}
	}
}

void update_wrapper(simple_tiling_utils::job_wrapper_inputs wrapper_inputs, simple_tiling_utils::update_job wrapped_job)
{
	ZoneScoped;
	XThreadWrapper::data& tileInfo = tile_data[wrapper_inputs.data].threadData;
	if (tileInfo.tile_running) // Avoid starting new jobs on terminated tiles
	{
		tileInfo.tile_state = simple_tiling_utils::PROCESSING; // No loops or swapchains - totes safe to enter PROCESSING on job start and return to IDLE on job resolve ( / wrapper return)
		wrapped_job(wrapper_inputs.data); // Updates are expected to take their tile index, but nothing else/
		tileInfo.tile_state = simple_tiling_utils::IDLE; // No loops or swapchains, so tiles go straight back to IDLE after completing
	}
}

void simple_tiling::submit_draw_work(simple_tiling_utils::draw_job work, uint64_t tile_mask)
{
	for (uint32_t i = 0; i < numTiles; i++) // For each tile
	{
		if (tile_mask & (1ull << i)) // Skip processing masked tiles
		{
			simple_tiling_utils::job_wrapper_inputs args;
			args.data = i;
			submit_draw_work_internal(draw_wrapper, args, work, i);
		}
		else if (tile_data[i].threadData.tile_state != simple_tiling_utils::UPLOADING) // Not sure how useful this is + feels problematic for performance ^_^'
		{
			// Not uploading or processing, so flag this tile as IDLE
			tile_data[i].threadData.tile_state = simple_tiling_utils::IDLE;
		}
	}
}

void simple_tiling::submit_update_work(simple_tiling_utils::update_job work, uint64_t tile_mask)
{
	for (uint32_t i = 0; i < numTiles; i++) // For each tile
	{
		if (tile_mask & (0x1ull << i)) // Skip processing masked tiles
		{
			simple_tiling_utils::job_wrapper_inputs args;
			args.data = i;
			submit_update_work_internal(update_wrapper, args, work, i);
		}
		else // No swapchain for updates, so else instead of else-if here ^_^
		{
			tile_data[i].threadData.tile_state = simple_tiling_utils::IDLE;
		}
	}
}

void thread_main(uint32_t tile_ndx)
{
	uint32_t frame_ctr = 0;
	XThreadWrapper::data& tile_info = tile_data[tile_ndx].threadData;
	while (tile_info.tile_running)
	{
		// Consume draw jobs, then consume update jobs
		// I don't *think* that should cause any issues
		// (no reason updates drawing during an upload would be a problem, unless the user is intentionally trying to make the main/tile threads interfere with each other)
		if (tile_draw_jobs[tile_ndx].front > 0)
		{
			tile_draw_jobs[tile_ndx].consume_job();
			tile_info.interlace_offset_x = frame_ctr % 2;
			tile_info.interlace_offset_y = (frame_ctr % 2) * NUM_VECTOR_LANES;
			frame_ctr++;
		}

		if (tile_update_jobs[tile_ndx].front > 0)
		{
			tile_update_jobs[tile_ndx].consume_job();
		}
	}
	tile_info.tile_shutdown_success = true;
}

void simple_tiling::WaitForTileProcessing(uint32_t tile_ndx)
{
	XThreadWrapper::data& tile_nfo = tile_data[tile_ndx].threadData;
	tile_nfo.tile_state.wait(simple_tiling_utils::PROCESSING); // Uncertain about this implementation - seems unstable for short-lived tasks or consumers using many tiles at once
}

void simple_tiling::WaitForTileUpload(uint32_t tile_ndx)
{
	XThreadWrapper::data& tile_nfo = tile_data[tile_ndx].threadData;
	tile_nfo.tile_state.wait(simple_tiling_utils::UPLOADING);
}

uint32_t simple_tiling::GetNumTilesTotal()
{
	return numTiles;
}

uint32_t simple_tiling::GetNumTilesX()
{
	return numTilesX;
}

uint32_t simple_tiling::GetNumTilesY()
{
	return numTilesY;
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
void simple_tiling::setup(uint32_t num_tiles, uint32_t window_width, uint32_t window_height, bool using_interlacing)
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
	XThreadWrapper::data* tileInfo = &tile_data[tileCtr].threadData;
	for (uint32_t x = 0; x < numTilesX; x++)
	{
		for (uint32_t y = 0; y < numTilesY; y++)
		{
			tileInfo->tileMinX = tile_width_px * x;
			tileInfo->tileMaxX = tileInfo->tileMinX + tile_width_px;

			tileInfo->tileMinY = tile_height_px * y;
			tileInfo->tileMaxY = tileInfo->tileMinY + tile_height_px;

			tileCtr++;
			tileInfo = &tile_data[tileCtr].threadData;
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
	interlacing = using_interlacing;
	for (uint32_t i = 0; i < num_tiles; i++)
	{
		tileBuffers[i] = alloc_array<simple_tiling_utils::color_batch>(tile_area_vectors);

		tile_data[i].threadData.tile_running = true;
		tile_data[i].threadData.tile_shutdown_success = false;
		tile_data[i].threadData.tile_state = simple_tiling_utils::IDLE;
		tile_data[i].threadData.blit_state = XThreadWrapper::COPIED;
		tile_data[i].threadData.interlace_offset_x = 0;
		tile_data[i].threadData.interlace_offset_y = 0;
		tile_data[i].threadData.tile = std::thread(thread_main, i);

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
		XThreadWrapper::data& tileInfo = tile_data[i].threadData;
		tileInfo.tile_running = false;
		while (!tileInfo.tile_shutdown_success)
		{
			// Repeatedly notify blocked threads until they unblock
			tileInfo.tile_state.store(simple_tiling_utils::IDLE);
			tileInfo.tile_state.notify_one();
		}
	}

	// Separate loops so that calls to [join] are delayed enough for tile states to be well-defined
	for (uint32_t i = 0; i < numTiles; i++)
	{
		XThreadWrapper::data& tileInfo = tile_data[i].threadData;
		tileInfo.tile.join();
	}

	// ... other shutdown things ... //
	free(tiling_pool); // <3 linear allocators
}

// Called from the WM_PAINT block of your message pump
// (between BeginPaint() and EndPaint())
void simple_tiling::win_paint(void* hdc)
{
	ZoneScoped;
	for (uint32_t i = 0; i < numTiles; i++)
	{
		XThreadWrapper::data& tileInfo = tile_data[i].threadData;

		// Spin here if the back-buffer is still blocked
		if (tileInfo.tile_state == simple_tiling_utils::UPLOADING)
		{
			tileInfo.blit_state = XThreadWrapper::AWAITING_COPY;
			tileInfo.blit_state.wait(XThreadWrapper::COPIED);
		}

		const uint32_t minY = tileInfo.tileMinY;
		const uint32_t minX = tileInfo.tileMinX;
		const uint32_t maxY = tileInfo.tileMaxY;
		const uint32_t maxX = tileInfo.tileMaxX;
		const uint32_t w = maxX - minX;
		const uint32_t h = maxY - minY;

		uint32_t test = SetDIBitsToDevice(static_cast<HDC>(hdc), minX, minY, w, h, minX, minY, minY, h, back_buffer, &canvas_bmi, DIB_RGB_COLORS);
		assert(test);
	}
}
