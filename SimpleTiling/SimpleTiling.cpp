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
#undef min
#undef max

#include <algorithm>
#include <concepts>
#include <condition_variable>

// Define statics declared in [SimpleTiling.h]
uint32_t canvas_width = 0;
uint32_t canvas_height = 0;
BITMAPINFO canvas_bmi;
uint32_t* back_buffer = nullptr;

// Our design goal is to automate tiling/thread scheduling, so that rendering apps can focus on their core details instead
namespace simple_tiling_utils
{
	// BEEG SOA port didn't really affect performance here; more work needed
	struct job_q
	{
		// Max number of queued jobs per-thread
		static constexpr int32_t max_queued_jobs = 16;

		// Draw & update job backlogs
		// void* for trashy C-style runtime polymorphism; valid casts are to/from draw_job and update_job
		// (depending on the value encoded in work_types for each job)
		// Bithacking to keep everything in cache instead of array explosion
		struct job_packet
		{
			// 48 bits original pointer data
			// 1 bit work-type
			// 1 bit sync mode
			uint64_t data;

			static constexpr uint64_t payload_mask = static_cast<uint64_t>(UINT8_MAX) << 56;
			static constexpr uint64_t address_mask = ~(static_cast<uint64_t>(UINT8_MAX) << 56);

			void decode(void*& address, WORK_TYPES& work_type, TASK_SYNC_TYPE& sync_mode)
			{
				address = reinterpret_cast<void*>(data & address_mask); // No need to worry about sign-extending with 1s; only working with userland pointers (whew)
				work_type = static_cast<WORK_TYPES>((data & (1ull << 63)) >> 63);
				sync_mode = static_cast<TASK_SYNC_TYPE>((data & (1ull << 62)) >> 62);
			}

			job_packet(void* address, WORK_TYPES work_type, TASK_SYNC_TYPE sync_mode)
			{
				data = reinterpret_cast<uint64_t>(address) & address_mask; // Address encoding
				data |= static_cast<uint64_t>(work_type) << 63; // Work type encoding
				data |= static_cast<uint64_t>(sync_mode) << 62; // Sync mode encoding
			}

			job_packet() : data(0) {}
		};
		job_packet jobs[max_queued_jobs * max_tiles] = {};

		// Wrappers for each job type
		draw_job_wrapper draw_wrapper;
		update_job_wrapper update_wrapper;

		// Book-keeping! Queue depth per-tile
		std::atomic_int front[max_tiles] = {};

		// More book-keeping; semaphores per-job to enable synchronisation
		std::atomic_int task_completion[max_queued_jobs * max_tiles] = {};

		void init_q(draw_job_wrapper _draw_wrapper, update_job_wrapper _update_wrapper)
		{
			ZeroMemory(jobs, sizeof(jobs));
			ZeroMemory(front, sizeof(front));
			ZeroMemory(task_completion, sizeof(task_completion));

			draw_wrapper = _draw_wrapper;
			update_wrapper = _update_wrapper;
		}

		template<typename job_type>
		void append_job(job_type job, uint32_t tile_count, WORK_TYPES work_type, TASK_SYNC_TYPE sync_mode, uint64_t tile_mask) requires (std::same_as<job_type, draw_job> || std::same_as<job_type, update_job>)
		{
			ZoneScoped;

			// SOA ring buffer
			// Can probably be optimized further by reformatting so wrappers/jobs/inputs can be memset - not up to that yet though

			// Sneaky math - instead of filling bits manually, over-shift and subtract to flush them to 1 automatically
			const bool tiles_filtered = tile_mask != UINT64_MAX && tile_mask != ((1ull << (tile_count + 1)) - 1);
			if (tiles_filtered)
			{
				for (uint32_t i = 0; i < tile_count; i++)
				{
					if (tile_mask & (1ull << i)) // Skip processing masked tiles
					{
						const uint32_t ndx = (i * max_queued_jobs) + (front[i] % max_queued_jobs);
						jobs[ndx] = job_packet(job, work_type, sync_mode);

						// Only set these atomics if we need to - polling them is expensive
						if (sync_mode == EXPLICIT_SYNC)
						{
							task_completion[ndx] = 0;
						}

						front[i]++;
					}
				}
			}
			else
			{
				for (uint32_t i = 0; i < tile_count; i++)
				{
					const uint32_t ndx = (i * max_queued_jobs) + (front[i] % max_queued_jobs);
					jobs[ndx] = job_packet(job, work_type, sync_mode);

					// Only set these atomics if we need to - polling them is expensive
					if (sync_mode == EXPLICIT_SYNC)
					{
						task_completion[ndx] = 0;
					}

					front[i]++;
				}
			}
		}

		void consume_job(uint32_t tile_ndx, uint32_t tile_count, WORK_TYPES* last_task_type)
		{
			ZoneScoped;

			// Only consume jobs if at least one is available in the queue; dip out if no consumeable work
			if (front == 0)
			{
				return;
			}

			// All good! Consume the most recently-submitted job
			const uint32_t ndx = std::max((front[tile_ndx] % max_queued_jobs) - 1, 0);
			const uint32_t offset = (tile_ndx * max_queued_jobs) + ndx;

			void* job;
			WORK_TYPES work_type;
			TASK_SYNC_TYPE sync_mode;
			jobs[offset].decode(job, work_type, sync_mode);

			// Ultra-hacky void* cast, but it's easier than anything else ^_^'
			if (work_type == DRAW_WORK)
			{
				draw_wrapper(tile_ndx, reinterpret_cast<draw_job>(job));
			}
			else
			{
				update_wrapper(tile_ndx, reinterpret_cast<update_job>(job));
			}

			// Flag task completed, wait for other threads if necessary
			// Both tasks are expensive, so only perform them if the current task uses the EXPLICIT_SYNC sync mode
			if (sync_mode == EXPLICIT_SYNC)
			{
				task_completion[offset]++;
				task_completion[offset].wait(tile_count);
			}

			front[tile_ndx]--;
			*last_task_type = work_type;
		}
	};
};

simple_tiling_utils::job_q tile_jobs = {};
simple_tiling_utils::color_batch* tileBuffers[simple_tiling_utils::max_tiles] = {};

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

bool interlacing = true;
void draw_wrapper(uint32_t tile_id, simple_tiling_utils::draw_job wrapped_job)
{
	ZoneScoped;
	XThreadWrapper::data& tileInfo = tile_data[tile_id].threadData;
	const uint32_t minX = tileInfo.tileMinX;
	const uint32_t maxX = tileInfo.tileMaxX;
	const uint32_t minY = tileInfo.tileMinY;
	const uint32_t maxY = tileInfo.tileMaxY;
	const uint8_t interlace_offs_x = tileInfo.interlace_offset_x;
	const uint8_t interlace_offs_y = tileInfo.interlace_offset_y;
	bool unblocked = false;

	// Need to de-interlace X and Y separately - otherwise one axis will always have gaps
	tileInfo.tile_state = simple_tiling_utils::PROCESSING;
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
			simple_tiling_utils::color_batch* batch_colors = tileBuffers[tile_id] + (tile_px / NUM_VECTOR_LANES);

			// Issue work
			const float init_px = static_cast<float>((pixel_row * canvas_width) + pixel_batch);
			wrapped_job(_mm256_set_ps(init_px, init_px + 1, init_px + 2, init_px + 3, init_px + 4, init_px + 5, init_px + 6, init_px + 7), tile_id, batch_colors);
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

			auto in_ptr = tileBuffers[tile_id];
			auto out_ptr = back_buffer + ((minY * canvas_width) + minX);

			for (uint32_t y = minY; y < maxY; y++)
			{
				memcpy(out_ptr, in_ptr, sizeof(uint32_t) * dest_w);
				in_ptr += src_w;
				out_ptr += canvas_width;
			}

			tileInfo.blit_state = XThreadWrapper::COPIED;

			// Signal the main thread that we've finished copying-out this tile, so the main thread can proceed with e.g. frame blits
			tileInfo.tile_state = simple_tiling_utils::IDLE;
			tileInfo.tile_state.notify_one();
		}
	}
}

void update_wrapper(uint32_t tile_id, simple_tiling_utils::update_job wrapped_job)
{
	ZoneScoped;
	XThreadWrapper::data& tileInfo = tile_data[tile_id].threadData;
	if (tileInfo.tile_running) // Avoid starting new jobs on terminated tiles
	{
		tileInfo.tile_state = simple_tiling_utils::PROCESSING; // No loops or swapchains - totes safe to enter PROCESSING on job start and return to IDLE on job resolve ( / wrapper return)
		wrapped_job(tile_id); // Updates are expected to take their tile index, but nothing else/
		tileInfo.tile_state = simple_tiling_utils::IDLE; // No loops or swapchains, so tiles go straight back to IDLE after completing
	}
}

void simple_tiling::submit_draw_work(simple_tiling_utils::draw_job work, simple_tiling_utils::TASK_SYNC_TYPE sync_mode, uint64_t tile_mask)
{
	ZoneScoped;
	tile_jobs.append_job(work, numTiles, simple_tiling_utils::DRAW_WORK, sync_mode, tile_mask);
}

void simple_tiling::submit_update_work(simple_tiling_utils::update_job work, simple_tiling_utils::TASK_SYNC_TYPE sync_mode, uint64_t tile_mask)
{
	ZoneScoped;
	tile_jobs.append_job(work, numTiles, simple_tiling_utils::UPDATE_WORK, sync_mode, tile_mask);
}

// Thread primitives controlling the producer thread (frame_main) & preventing it from supplying too much work to tile threads
// (executing thread_main)
std::atomic_bool producer_resting = false;
std::mutex frame_pacer_mutex;
std::condition_variable frame_pacer;

void thread_main(uint32_t tile_ndx)
{
	uint32_t tick_ctr = 0;
	XThreadWrapper::data& tile_info = tile_data[tile_ndx].threadData;
	while (tile_info.tile_running)
	{
		// Consume draw jobs, then consume update jobs
		// I don't *think* that should cause any issues
		// (no reason updates drawing during an upload would be a problem, unless the user is intentionally trying to make the main/tile threads interfere with each other)
		const uint32_t job_count = tile_jobs.front[tile_ndx];
		if (job_count > 0)
		{
			const uint32_t work_type_offset = (tile_ndx * simple_tiling_utils::job_q::max_queued_jobs) + job_count;
			simple_tiling_utils::WORK_TYPES last_job_type;
			tile_jobs.consume_job(tile_ndx, numTiles, &last_job_type);

			// Only iterate interlacing for draw tasks - ignore for update work
			if (last_job_type == simple_tiling_utils::DRAW_WORK)
			{
				tile_info.interlace_offset_x = tick_ctr % 2;
				tile_info.interlace_offset_y = (tick_ctr % 2) * NUM_VECTOR_LANES;
				tick_ctr++;
			}
		}
	}
	tile_info.tile_shutdown_success = true;
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

	tile_jobs.init_q(draw_wrapper, update_wrapper);

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
void simple_tiling::win_paint(void* hdc, uint32_t frame_budget_ms)
{
	ZoneScoped;
	auto t = std::chrono::steady_clock::now();
	auto start_t = t.time_since_epoch();
	uint32_t d_i = 1;
	for (uint32_t i = 0; i < numTiles; i += d_i)
	{
		ZoneScoped;
		XThreadWrapper::data& tileInfo = tile_data[i].threadData;

		// Spin here if the back-buffer is still blocked
		if (tileInfo.tile_state == simple_tiling_utils::UPLOADING)
		{
			tileInfo.blit_state = XThreadWrapper::AWAITING_COPY;
		}
		else
		{
			ZoneScoped;
			const uint32_t minY = tileInfo.tileMinY;
			const uint32_t minX = tileInfo.tileMinX;
			const uint32_t maxY = tileInfo.tileMaxY;
			const uint32_t h = maxY - minY;
			uint32_t maxX = tileInfo.tileMaxX;

			// If the current tile & the following tiles have the same y-value, they're adjacent (because of the logic we used to lay-out the tiles when we placed them)
			// In that case, check if each of the following tiles are also not being uploaded and copy them out with the current one; that may be faster than many separate copies
			uint32_t skipped_ctr = 1;
			bool searching = true;
			while (searching)
			{
				const XThreadWrapper::data& nextTileInfo = tile_data[i + skipped_ctr].threadData;
				const uint32_t nextMinY = nextTileInfo.tileMinY;
				const uint32_t nextBlitState = nextTileInfo.blit_state;

				if (nextMinY == minY && nextBlitState == tileInfo.blit_state)
				{
					d_i = skipped_ctr;
					maxX = nextTileInfo.tileMaxX;
					skipped_ctr++;
				}
				else
				{
					searching = false;
					break;
				}
			}

			const uint32_t w = maxX - minX;
			uint32_t test = SetDIBitsToDevice(static_cast<HDC>(hdc), minX, minY, w, h, minX, minY, minY, h, back_buffer, &canvas_bmi, DIB_RGB_COLORS);
			assert(test);
		}

		t = std::chrono::steady_clock::now();
		const auto curr_t = t.time_since_epoch();
		const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(curr_t - start_t);
		if (dt_ms.count() > frame_budget_ms)
		{
			break;
		}
	}
}
