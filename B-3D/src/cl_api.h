
#ifndef B3D_CL_API_H
#define B3D_CL_API_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef uint64_t cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_command_queue_properties;
typedef cl_uint cl_bool;
typedef cl_uint cl_platform_info;
typedef cl_uint cl_device_info;
typedef cl_uint cl_program_build_info;
typedef cl_uint cl_kernel_work_group_info;

typedef struct _cl_platform_id *cl_platform_id;
typedef struct _cl_device_id *cl_device_id;
typedef struct _cl_context *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_mem *cl_mem;
typedef struct _cl_program *cl_program;
typedef struct _cl_kernel *cl_kernel;
typedef struct _cl_event *cl_event;
typedef intptr_t cl_context_properties;

#define CL_SUCCESS 0
#define CL_FALSE 0
#define CL_TRUE 1

#define CL_DEVICE_TYPE_CPU (1 << 1)
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_DEVICE_TYPE_ACCELERATOR (1 << 3)
#define CL_DEVICE_TYPE_ALL 0xFFFFFFFFu

#define CL_PLATFORM_NAME 0x0902

#define CL_DEVICE_TYPE 0x1000
#define CL_DEVICE_MAX_COMPUTE_UNITS 0x1002
#define CL_DEVICE_MAX_MEM_ALLOC_SIZE 0x1010
#define CL_DEVICE_GLOBAL_MEM_SIZE 0x101F
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C
#define CL_DEVICE_VERSION 0x102F
#define CL_DEVICE_AVAILABLE 0x1027
#define CL_DEVICE_COMPILER_AVAILABLE 0x1028

#define CL_MEM_READ_WRITE (1 << 0)
#define CL_MEM_WRITE_ONLY (1 << 1)
#define CL_MEM_READ_ONLY (1 << 2)
#define CL_MEM_COPY_HOST_PTR (1 << 5)

#define CL_PROGRAM_BUILD_LOG 0x1183
#define CL_KERNEL_WORK_GROUP_SIZE 0x11B0

#define B3D_CL_FUNCS                                                                                                                                                \
   X(cl_int, clGetPlatformIDs, (cl_uint, cl_platform_id *, cl_uint *))                                                                                              \
   X(cl_int, clGetPlatformInfo, (cl_platform_id, cl_platform_info, size_t, void *, size_t *))                                                                       \
   X(cl_int, clGetDeviceIDs, (cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *))                                                                  \
   X(cl_int, clGetDeviceInfo, (cl_device_id, cl_device_info, size_t, void *, size_t *))                                                                             \
   X(cl_context, clCreateContext, (const cl_context_properties *, cl_uint, const cl_device_id *, void *, void *, cl_int *))                                         \
   X(cl_command_queue, clCreateCommandQueue, (cl_context, cl_device_id, cl_command_queue_properties, cl_int *))                                                     \
   X(cl_program, clCreateProgramWithSource, (cl_context, cl_uint, const char **, const size_t *, cl_int *))                                                         \
   X(cl_int, clBuildProgram, (cl_program, cl_uint, const cl_device_id *, const char *, void *, void *))                                                             \
   X(cl_int, clGetProgramBuildInfo, (cl_program, cl_device_id, cl_program_build_info, size_t, void *, size_t *))                                                    \
   X(cl_kernel, clCreateKernel, (cl_program, const char *, cl_int *))                                                                                               \
   X(cl_int, clSetKernelArg, (cl_kernel, cl_uint, size_t, const void *))                                                                                            \
   X(cl_int, clGetKernelWorkGroupInfo, (cl_kernel, cl_device_id, cl_kernel_work_group_info, size_t, void *, size_t *))                                              \
   X(cl_mem, clCreateBuffer, (cl_context, cl_mem_flags, size_t, void *, cl_int *))                                                                                  \
   X(cl_int, clEnqueueWriteBuffer, (cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *))                        \
   X(cl_int, clEnqueueReadBuffer, (cl_command_queue, cl_mem, cl_bool, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *))                               \
   X(cl_int, clEnqueueNDRangeKernel, (cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *)) \
   X(cl_int, clFinish, (cl_command_queue))                                                                                                                          \
   X(cl_int, clReleaseMemObject, (cl_mem))                                                                                                                          \
   X(cl_int, clReleaseKernel, (cl_kernel))                                                                                                                          \
   X(cl_int, clReleaseProgram, (cl_program))                                                                                                                        \
   X(cl_int, clReleaseCommandQueue, (cl_command_queue))                                                                                                             \
   X(cl_int, clReleaseContext, (cl_context))

#endif
