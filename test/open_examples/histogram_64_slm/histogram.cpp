/*
 * Copyright (c) 2017, Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "cm_rt.h"

// Includes bitmap_helpers.h for bitmap file open/save operations.
#include "common/bitmap_helpers.h"

// Includes cm_rt_helpers.h to convert the integer return code returned from
// the CM runtime to a meaningful string message.
#include "common/cm_rt_helpers.h"

// Includes isa_helpers.h to load the ISA file generated by the CM compiler.
#include "common/isa_helpers.h"

#define NUM_BINS     64
#define BLOCK_WIDTH  32
#define BLOCK_HEIGHT 512

// This function calculates histogram of the image with the CPU.
// @param size: the size of the input array.
// @param src: pointer to the input array.
// @param cpu_histogram: pointer to the histogram of the input image.
void HistogramCPU(unsigned int size,
                  unsigned int *src,
                  unsigned int *cpu_histogram) {
    for (int i = 0; i < size; i++) {
        unsigned int x = src[i];
        cpu_histogram[(x >> 2) & 0x3FU] += 1;
        cpu_histogram[(x >> 10) & 0x3FU] += 1;
        cpu_histogram[(x >> 18) & 0x3FU] += 1;
        cpu_histogram[(x >> 26) & 0x3FU] += 1;
    }
}

// This function compares the output data calculated by the CPU and the
// GPU separately.
// If they are identical, return 1, else return 0.
int CheckHistogram(unsigned int *cpu_histogram,
                   unsigned int *gpu_histogram) {
    for (int i = 0; i < NUM_BINS; i++) {
        if (cpu_histogram[i] != gpu_histogram[i]) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char *argv[]) {
    const char *input_file = nullptr;
    unsigned int width = 4096 * sizeof(unsigned int);
    unsigned int height = 4096;

    if (argc == 2) {
      input_file = argv[1];
    }
    else {
      std::cerr << "Usage: Histogram.exe input_file" << std::endl;
      std::cerr << "No input file specificed. Use default random value ...." << std::endl;
    }

    // Initializes input.
    unsigned int input_size = width * height / sizeof(unsigned int);
    unsigned int *input_ptr = (unsigned int*)CM_ALIGNED_MALLOC(input_size*sizeof(unsigned int), 2 * 1024 * 1024);
    printf("Processing %dx%d inputs\n", width / sizeof(unsigned int), height);

    if (input_file != nullptr) {
      FILE *f = fopen(input_file, "rb");
      if (f == NULL) {
        fprintf(stderr, "Error opening file %s", input_file);
        std::exit(1);
      }
      if (fread(input_ptr, sizeof(unsigned int), input_size, f) != input_size) {
        fprintf(stderr, "Error reading input from %s\n", input_file);
        std::exit(1);
      }
    }
    else {
      srand(2009);
      for (int i = 0; i < input_size; ++i)
      {
        input_ptr[i] = rand() % 256;
        input_ptr[i] |= (rand() % 256) << 8;
        input_ptr[i] |= (rand() % 256) << 16;
        input_ptr[i] |= (rand() % 256) << 24;
      }
    }

    double   exec_start, exec_stop;
    float    exec_total = 0.0f;
    const int NUM_ITER = 101;

    // Allocates system memory for output buffer.
    int buffer_size = sizeof(unsigned int) * NUM_BINS;
    unsigned int *hist = new unsigned int[buffer_size];
    if (hist == nullptr) {
        fprintf(stderr, "Out of memory");
        exit(1);
    }
    memset(hist, 0, buffer_size);

    // Uses the CPU to calculate the histogram output data.
    unsigned int cpu_histogram[NUM_BINS];
    memset(cpu_histogram, 0, sizeof(cpu_histogram));
    HistogramCPU(input_size, input_ptr, cpu_histogram);

    // Uses the GPU to calculate the histogram output data.
    // Creates a CmDevice from scratch.
    // Param device: pointer to the CmDevice object.
    // Param version: CM API version supported by the runtime library.
    CmDevice *device = nullptr;;
    unsigned int version = 0;
    cm_result_check(::CreateCmDevice(device, version));

    // The file histogram_atomic_genx.isa is generated when the kernel in the
    // file histogram_atomic_genx.cpp are compiled by the CM compiler.
    // Reads in the virtual ISA from "histogram_atomic_genx.isa" to the code
    // buffer.
    std::string isa_code = cm::util::isa::loadFile("histogram_genx.isa");
    if (isa_code.size() == 0) {
        std::cerr << "Error: empty ISA binary.\n";
        std::exit(1);
    }

    // Creates a CmProgram object consisting of the kernels loaded from the code
    // buffer.
    // Param isa_code.data(): Pointer to the code buffer containing the virtual
    // ISA.
    // Param isa_code.size(): Size in bytes of the code buffer containing the
    // virtual ISA.
    CmProgram *program = nullptr;
    cm_result_check(device->LoadProgram(const_cast<char *>(isa_code.data()),
                                        isa_code.size(),
                                        program));

    // Creates the kernel.
    // Param program: CM Program from which the kernel is created.
    // Param "histogram_atomic": The kernel name which should be no more than 256
    // bytes including the null terminator.
    CmKernel *kernel = nullptr;
    cm_result_check(device->CreateKernel(program,
                                         "histogram_atomic",
                                         kernel));

    // Creates input surface with given width and height in pixels and format.
    CmSurface2D *input_surface = nullptr;
    cm_result_check(device->CreateSurface2D(width,
                                            height,
                                            CM_SURFACE_FORMAT_A8,
                                            input_surface));

    // Copies system memory content to the input surface using the CPU. The
    // system memory content is the data of the input image. The size of data
    // copied is the size of data in the surface.
    cm_result_check(input_surface->WriteSurface((unsigned char *)input_ptr, nullptr));

    // Creates a CmBuffer with the specified size in bytes.
    // CmBuffer represents a 1D surface in video memory.
    // This buffer contains the output of the GPU.
    CmBuffer *output_surface[NUM_ITER] = { nullptr };
    for (int i = 0; i < NUM_ITER; i++) {
        cm_result_check(device->CreateBuffer(4 * NUM_BINS, output_surface[i]));
    }

    // Creates a CmThreadSpace object.
    unsigned int ts_width, ts_height;
    ts_width = width / BLOCK_WIDTH;
    ts_height = height / BLOCK_HEIGHT;
    CmThreadGroupSpace *thread_space = nullptr;
    cm_result_check(device->CreateThreadGroupSpace(1, 1, ts_width, ts_height, thread_space));

    // Creates a task queue.
    // The CmQueue is an in-order queue. Tasks get executed according to the
    // order they are enqueued. The next task does not start execution until the
    // current task finishes.
    CmQueue *cmd_queue = nullptr;
    cm_result_check(device->CreateQueue(cmd_queue));

    // Creates a CmTask object.
    // The CmTask object is a container for CmKernel pointers. It is used to
    // enqueue the kernels for execution.
    CmTask *task = nullptr;
    cm_result_check(device->CreateTask(task));

    // Adds a CmKernel pointer to CmTask.
    cm_result_check(task->AddKernel(kernel));

    // When a surface is created by the CmDevice a SurfaceIndex object is
    // created. This object contains a unique index value that is mapped to
    // the surface.
    // Gets the input surface index.
    SurfaceIndex *input_surface_idx = nullptr;
    cm_result_check(input_surface->GetIndex(input_surface_idx));

    // Gets the output surface index.
    SurfaceIndex *output_surface_idx[NUM_ITER] = { nullptr };
    for (int i = 0; i < NUM_ITER; i++) {
        cm_result_check(output_surface[i]->GetIndex(output_surface_idx[i]));
    }

    CmEvent *sync_events[NUM_ITER];
    for (int i = 0; i < NUM_ITER; i++) {
        if (i == 1) {
            exec_start = getTimeStamp();
        }
        // Sets a per kernel argument.
        // Sets input surface index as the first argument of kernel.
        // Sets output surface index as the second argument of kernel.
        cm_result_check(kernel->SetKernelArg(0,
          sizeof(SurfaceIndex),
          input_surface_idx));
        cm_result_check(kernel->SetKernelArg(1,
          sizeof(SurfaceIndex),
          output_surface_idx[i]));

        // Launches the task on the GPU. Enqueue is a non-blocking call, i.e. the
        // function returns immediately without waiting for the GPU to start or
        // finish execution of the task. The runtime will query the HW status. If
        // the hardware is not busy, the runtime will submit the task to the
        // driver/HW; otherwise, the runtime will submit the task to the driver/HW
        // at another time.
        // An event, "sync_event", is created to track the status of the task.
	      sync_events[i] = nullptr;
        cm_result_check(cmd_queue->EnqueueWithGroup(task, sync_events[i], thread_space));
    }
    DWORD dwTimeOutMs = -1;
    cm_result_check((sync_events[NUM_ITER-1])->WaitForTaskFinished(dwTimeOutMs));

    exec_stop = getTimeStamp();
    exec_total = float(exec_stop - exec_start);

    // Destroys a CmTask object.
    // CmTask will be destroyed when CmDevice is destroyed.
    // Here, the application destroys the CmTask object by itself.
    cm_result_check(device->DestroyTask(task));

    // Destroy a CmThreadSpace object.
    // CmThreadSpace will be destroyed when CmDevice is destroyed.
    // Here, the application destroys the CmThreadSpace object by itself.
    cm_result_check(device->DestroyThreadGroupSpace(thread_space));

    // Reads the output surface content to the system memory using the CPU.
    // The size of data copied is the size of data in Surface.
    // It is a blocking call. The function will not return until the copy
    // operation is completed.
    // The dependent event "sync_event" ensures that the reading of the surface
    // will not happen until its state becomes CM_STATUS_FINISHED.
    cm_result_check(output_surface[NUM_ITER - 1]->ReadSurface((unsigned char *)hist,
                                                sync_events[NUM_ITER-1]));

    UINT64 total_time = 0;
    for (int i = 1; i < NUM_ITER; i++) {
        UINT64 execution_time = 0;
        sync_events[i]->GetExecutionTime(execution_time);
        total_time += execution_time;
    }

    int count = NUM_ITER - 1;
    std::cout << "Kernel Histogram execution time is " << (total_time / 1000000.0f / count) << " msec" << std::endl;
    std::cout << "Total time is " << (1000.0f * exec_total / count) << " msec" << std::endl;
    std::cout << "Total Iteration count is " << count << std::endl;

    // Destroys the CmDevice.
    // Also destroys surfaces, kernels, tasks, thread spaces, and queues that
    // were created using this device instance that have not explicitly been
    // destroyed by calling the respective destroy functions.
    cm_result_check(::DestroyCmDevice(device));

    // Frees memory.
    CM_ALIGNED_FREE(input_ptr);

    // Compares the CPU histogram output data with the GPU histogram output data.
    // If there is no difference, the result is correct.
    // Otherwise there is something wrong.
    if (CheckHistogram(cpu_histogram, hist)) {
        printf("PASSED\n");
        delete[] hist;
        return 0;
    } else {
        printf("FAILED\n");
        delete[] hist;
        return -1;
    }
}
