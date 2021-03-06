/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015,2016,2017, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
#include "gmxpre.h"

#include "detecthardware.h"

#include "config.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "thread_mpi/threads.h"

#include "gromacs/gpu_utils/gpu_utils.h"
#include "gromacs/hardware/cpuinfo.h"
#include "gromacs/hardware/gpu_hw_info.h"
#include "gromacs/hardware/hardwaretopology.h"
#include "gromacs/hardware/hw_info.h"
#include "gromacs/mdtypes/commrec.h"
#include "gromacs/simd/support.h"
#include "gromacs/utility/basedefinitions.h"
#include "gromacs/utility/basenetwork.h"
#include "gromacs/utility/baseversion.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/logger.h"
#include "gromacs/utility/programcontext.h"
#include "gromacs/utility/smalloc.h"
#include "gromacs/utility/stringutil.h"
#include "gromacs/utility/sysinfo.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>       // sysconf()
#endif

//! Convenience macro to help us avoid ifdefs each time we use sysconf
#if !defined(_SC_NPROCESSORS_ONLN) && defined(_SC_NPROC_ONLN)
#    define _SC_NPROCESSORS_ONLN _SC_NPROC_ONLN
#endif

//! Convenience macro to help us avoid ifdefs each time we use sysconf
#if !defined(_SC_NPROCESSORS_CONF) && defined(_SC_NPROC_CONF)
#    define _SC_NPROCESSORS_CONF _SC_NPROC_CONF
#endif

#if defined (__i386__) || defined (__x86_64__) || defined (_M_IX86) || defined (_M_X64)
//! Constant used to help minimize preprocessed code
static const bool isX86 = true;
#else
//! Constant used to help minimize preprocessed code
static const bool isX86 = false;
#endif

#if defined __powerpc__ || defined __ppc__ || defined __PPC__
static const bool isPowerPC = true;
#else
static const bool isPowerPC = false;
#endif

//! Constant used to help minimize preprocessed code
static const bool bGPUBinary     = GMX_GPU != GMX_GPU_NONE;

/* Note that some of the following arrays must match the "GPU support
 * enumeration" in src/config.h.cmakein, so that GMX_GPU looks up an
 * array entry. */

// TODO If/when we unify CUDA and OpenCL support code, this should
// move to a single place in gpu_utils.
/* Names of the GPU detection/check results (see e_gpu_detect_res_t in hw_info.h). */
const char * const gpu_detect_res_str[egpuNR] =
{
    "compatible", "inexistent", "incompatible", "insane"
};

/* The globally shared hwinfo structure. */
static gmx_hw_info_t      *hwinfo_g;
/* A reference counter for the hwinfo structure */
static int                 n_hwinfo = 0;
/* A lock to protect the hwinfo structure */
static tMPI_Thread_mutex_t hw_info_lock = TMPI_THREAD_MUTEX_INITIALIZER;

static void gmx_detect_gpus(const gmx::MDLogger &mdlog, const t_commrec *cr)
{
#if GMX_LIB_MPI
    int              rank_world;
    MPI_Comm         physicalnode_comm;
#endif
    int              rank_local;

    /* Under certain circumstances MPI ranks on the same physical node
     * can not simultaneously access the same GPU(s). Therefore we run
     * the detection only on one MPI rank per node and broadcast the info.
     * Note that with thread-MPI only a single thread runs this code.
     *
     * NOTE: We can't broadcast gpu_info with OpenCL as the device and platform
     * ID stored in the structure are unique for each rank (even if a device
     * is shared by multiple ranks).
     *
     * TODO: We should also do CPU hardware detection only once on each
     * physical node and broadcast it, instead of do it on every MPI rank.
     */
#if GMX_LIB_MPI
    /* A split of MPI_COMM_WORLD over physical nodes is only required here,
     * so we create and destroy it locally.
     */
    MPI_Comm_rank(MPI_COMM_WORLD, &rank_world);
    MPI_Comm_split(MPI_COMM_WORLD, gmx_physicalnode_id_hash(),
                   rank_world, &physicalnode_comm);
    MPI_Comm_rank(physicalnode_comm, &rank_local);
    GMX_UNUSED_VALUE(cr);
#else
    /* Here there should be only one process, check this */
    GMX_RELEASE_ASSERT(cr->nnodes == 1 && cr->sim_nodeid == 0, "Only a single (master) process should execute here");

    rank_local = 0;
#endif

    /*  With CUDA detect only on one rank per host, with OpenCL need do
     *  the detection on all PP ranks */
    bool isOpenclPpRank = ((GMX_GPU == GMX_GPU_OPENCL) && (cr->duty & DUTY_PP));

    if (rank_local == 0 || isOpenclPpRank)
    {
        char detection_error[STRLEN] = "", sbuf[STRLEN];

        if (detect_gpus(&hwinfo_g->gpu_info, detection_error) != 0)
        {
            if (detection_error[0] != '\0')
            {
                sprintf(sbuf, ":\n      %s\n", detection_error);
            }
            else
            {
                sprintf(sbuf, ".");
            }
            GMX_LOG(mdlog.warning).asParagraph().appendTextFormatted(
                    "NOTE: Error occurred during GPU detection%s"
                    "      Can not use GPU acceleration, will fall back to CPU kernels.",
                    sbuf);
        }
    }

#if GMX_LIB_MPI
    if (!isOpenclPpRank)
    {
        /* Broadcast the GPU info to the other ranks within this node */
        MPI_Bcast(&hwinfo_g->gpu_info.n_dev, 1, MPI_INT, 0, physicalnode_comm);

        if (hwinfo_g->gpu_info.n_dev > 0)
        {
            int dev_size;

            dev_size = hwinfo_g->gpu_info.n_dev*sizeof_gpu_dev_info();

            if (rank_local > 0)
            {
                hwinfo_g->gpu_info.gpu_dev =
                    (struct gmx_device_info_t *)malloc(dev_size);
            }
            MPI_Bcast(hwinfo_g->gpu_info.gpu_dev, dev_size, MPI_BYTE,
                      0, physicalnode_comm);
            MPI_Bcast(&hwinfo_g->gpu_info.n_dev_compatible, 1, MPI_INT,
                      0, physicalnode_comm);
        }
    }

    MPI_Comm_free(&physicalnode_comm);
#endif
}

static void gmx_collect_hardware_mpi(const gmx::CpuInfo &cpuInfo)
{
    const int ncore = hwinfo_g->hardwareTopology->numberOfCores();
#if GMX_LIB_MPI
    int       rank_id;
    int       nrank, rank, nhwthread, ngpu, i;
    int       gpu_hash;
    int      *buf, *all;

    rank_id   = gmx_physicalnode_id_hash();
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nrank);
    nhwthread = hwinfo_g->nthreads_hw_avail;
    ngpu      = hwinfo_g->gpu_info.n_dev_compatible;
    /* Create a unique hash of the GPU type(s) in this node */
    gpu_hash  = 0;
    /* Here it might be better to only loop over the compatible GPU, but we
     * don't have that information available and it would also require
     * removing the device ID from the device info string.
     */
    for (i = 0; i < hwinfo_g->gpu_info.n_dev; i++)
    {
        char stmp[STRLEN];

        /* Since the device ID is incorporated in the hash, the order of
         * the GPUs affects the hash. Also two identical GPUs won't give
         * a gpu_hash of zero after XORing.
         */
        get_gpu_device_info_string(stmp, hwinfo_g->gpu_info, i);
        gpu_hash ^= gmx_string_fullhash_func(stmp, gmx_string_hash_init);
    }

    snew(buf, nrank);
    snew(all, nrank);
    buf[rank] = rank_id;

    MPI_Allreduce(buf, all, nrank, MPI_INT, MPI_SUM, MPI_COMM_WORLD);

    gmx_bool bFound;
    int      nnode0, ncore0, nhwthread0, ngpu0, r;

    bFound     = FALSE;
    ncore0     = 0;
    nnode0     = 0;
    nhwthread0 = 0;
    ngpu0      = 0;
    for (r = 0; r < nrank; r++)
    {
        if (all[r] == rank_id)
        {
            if (!bFound && r == rank)
            {
                /* We are the first rank in this physical node */
                nnode0     = 1;
                ncore0     = ncore;
                nhwthread0 = nhwthread;
                ngpu0      = ngpu;
            }
            bFound = TRUE;
        }
    }

    sfree(buf);
    sfree(all);

    int sum[4], maxmin[10];

    {
        int buf[4];

        /* Sum values from only intra-rank 0 so we get the sum over all nodes */
        buf[0] = nnode0;
        buf[1] = ncore0;
        buf[2] = nhwthread0;
        buf[3] = ngpu0;

        MPI_Allreduce(buf, sum, 4, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    }

    {
        int buf[10];

        /* Store + and - values for all ranks,
         * so we can get max+min with one MPI call.
         */
        buf[0] = ncore;
        buf[1] = nhwthread;
        buf[2] = ngpu;
        buf[3] = static_cast<int>(gmx::simdSuggested(cpuInfo));
        buf[4] = gpu_hash;
        buf[5] = -buf[0];
        buf[6] = -buf[1];
        buf[7] = -buf[2];
        buf[8] = -buf[3];
        buf[9] = -buf[4];

        MPI_Allreduce(buf, maxmin, 10, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    }

    hwinfo_g->nphysicalnode       = sum[0];
    hwinfo_g->ncore_tot           = sum[1];
    hwinfo_g->ncore_min           = -maxmin[5];
    hwinfo_g->ncore_max           = maxmin[0];
    hwinfo_g->nhwthread_tot       = sum[2];
    hwinfo_g->nhwthread_min       = -maxmin[6];
    hwinfo_g->nhwthread_max       = maxmin[1];
    hwinfo_g->ngpu_compatible_tot = sum[3];
    hwinfo_g->ngpu_compatible_min = -maxmin[7];
    hwinfo_g->ngpu_compatible_max = maxmin[2];
    hwinfo_g->simd_suggest_min    = -maxmin[8];
    hwinfo_g->simd_suggest_max    = maxmin[3];
    hwinfo_g->bIdenticalGPUs      = (maxmin[4] == -maxmin[9]);
#else
    /* All ranks use the same pointer, protected by a mutex in the caller */
    hwinfo_g->nphysicalnode       = 1;
    hwinfo_g->ncore_tot           = ncore;
    hwinfo_g->ncore_min           = ncore;
    hwinfo_g->ncore_max           = ncore;
    hwinfo_g->nhwthread_tot       = hwinfo_g->nthreads_hw_avail;
    hwinfo_g->nhwthread_min       = hwinfo_g->nthreads_hw_avail;
    hwinfo_g->nhwthread_max       = hwinfo_g->nthreads_hw_avail;
    hwinfo_g->ngpu_compatible_tot = hwinfo_g->gpu_info.n_dev_compatible;
    hwinfo_g->ngpu_compatible_min = hwinfo_g->gpu_info.n_dev_compatible;
    hwinfo_g->ngpu_compatible_max = hwinfo_g->gpu_info.n_dev_compatible;
    hwinfo_g->simd_suggest_min    = static_cast<int>(simdSuggested(cpuInfo));
    hwinfo_g->simd_suggest_max    = static_cast<int>(simdSuggested(cpuInfo));
    hwinfo_g->bIdenticalGPUs      = TRUE;
#endif
}

/*! \brief Utility that does dummy computing for max 2 seconds to spin up cores
 *
 *  This routine will check the number of cores configured and online
 *  (using sysconf), and the spins doing dummy compute operations for up to
 *  2 seconds, or until all cores have come online. This can be used prior to
 *  hardware detection for platforms that take unused processors offline.
 *
 *  This routine will not throw exceptions.
 */
static void
spinUpCore() noexcept
{
#if defined(HAVE_SYSCONF) && defined(_SC_NPROCESSORS_CONF) && defined(_SC_NPROCESSORS_ONLN)
    float dummy           = 0.1;
    int   countConfigured = sysconf(_SC_NPROCESSORS_CONF);    // noexcept
    auto  start           = std::chrono::steady_clock::now(); // noexcept

    while (sysconf(_SC_NPROCESSORS_ONLN) < countConfigured &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
    {
        for (int i = 1; i < 10000; i++)
        {
            dummy /= i;
        }
    }

    if (dummy < 0)
    {
        printf("This cannot happen, but prevents loop from being optimized away.");
    }
#endif
}

/*! \brief Prepare the system before hardware topology detection
 *
 * This routine should perform any actions we want to put the system in a state
 * where we want it to be before detecting the hardware topology. For most
 * processors there is nothing to do, but some architectures (in particular ARM)
 * have support for taking configured cores offline, which will make them disappear
 * from the online processor count.
 *
 * This routine checks if there is a mismatch between the number of cores
 * configured and online, and in that case we issue a small workload that
 * attempts to wake sleeping cores before doing the actual detection.
 *
 * This type of mismatch can also occur for x86 or PowerPC on Linux, if SMT has only
 * been disabled in the kernel (rather than bios). Since those cores will never
 * come online automatically, we currently skip this test for x86 & PowerPC to
 * avoid wasting 2 seconds. We also skip the test if there is no thread support.
 *
 * \note Cores will sleep relatively quickly again, so it's important to issue
 *       the real detection code directly after this routine.
 */
static void
hardwareTopologyPrepareDetection()
{
#if defined(HAVE_SYSCONF) && defined(_SC_NPROCESSORS_CONF) && \
    (defined(THREAD_PTHREADS) || defined(THREAD_WINDOWS))

    // Modify this conditional when/if x86 or PowerPC starts to sleep some cores
    if (!isX86 && !isPowerPC)
    {
        int                      countConfigured  = sysconf(_SC_NPROCESSORS_CONF);
        std::vector<std::thread> workThreads(countConfigured);

        for (auto &t : workThreads)
        {
            t = std::thread(spinUpCore);
        }

        for (auto &t : workThreads)
        {
            t.join();
        }
    }
#endif
}

/*! \brief Sanity check hardware topology and print some notes to log
 *
 *  \param mdlog            Logger.
 *  \param hardwareTopology Reference to hardwareTopology object.
 */
static void
hardwareTopologyDoubleCheckDetection(const gmx::MDLogger gmx_unused         &mdlog,
                                     const gmx::HardwareTopology gmx_unused &hardwareTopology)
{
#if defined HAVE_SYSCONF && defined(_SC_NPROCESSORS_CONF)
    if (hardwareTopology.supportLevel() < gmx::HardwareTopology::SupportLevel::LogicalProcessorCount)
    {
        return;
    }

    int countFromDetection = hardwareTopology.machine().logicalProcessorCount;
    int countConfigured    = sysconf(_SC_NPROCESSORS_CONF);

    /* BIOS, kernel or user actions can take physical processors
     * offline. We already cater for the some of the cases inside the hardwareToplogy
     * by trying to spin up cores just before we detect, but there could be other
     * cases where it is worthwhile to hint that there might be more resources available.
     */
    if (countConfigured >= 0 && countConfigured != countFromDetection)
    {
        GMX_LOG(mdlog.info).
            appendTextFormatted("Note: %d CPUs configured, but only %d were detected to be online.\n", countConfigured, countFromDetection);

        if (isX86 && countConfigured == 2*countFromDetection)
        {
            GMX_LOG(mdlog.info).
                appendText("      X86 Hyperthreading is likely disabled; enable it for better performance.");
        }
        // For PowerPC (likely Power8) it is possible to set SMT to either 2,4, or 8-way hardware threads.
        // We only warn if it is completely disabled since default performance drops with SMT8.
        if (isPowerPC && countConfigured == 8*countFromDetection)
        {
            GMX_LOG(mdlog.info).
                appendText("      PowerPC SMT is likely disabled; enable SMT2/SMT4 for better performance.");
        }
    }
#endif
}


gmx_hw_info_t *gmx_detect_hardware(const gmx::MDLogger &mdlog, const t_commrec *cr,
                                   gmx_bool bDetectGPUs)
{
    int ret;

    /* make sure no one else is doing the same thing */
    ret = tMPI_Thread_mutex_lock(&hw_info_lock);
    if (ret != 0)
    {
        gmx_fatal(FARGS, "Error locking hwinfo mutex: %s", strerror(errno));
    }

    /* only initialize the hwinfo structure if it is not already initalized */
    if (n_hwinfo == 0)
    {
        snew(hwinfo_g, 1);

        hwinfo_g->cpuInfo             = new gmx::CpuInfo(gmx::CpuInfo::detect());

        hardwareTopologyPrepareDetection();
        hwinfo_g->hardwareTopology    = new gmx::HardwareTopology(gmx::HardwareTopology::detect());

        // If we detected the topology on this system, double-check that it makes sense
        if (hwinfo_g->hardwareTopology->isThisSystem())
        {
            hardwareTopologyDoubleCheckDetection(mdlog, *(hwinfo_g->hardwareTopology));
        }

        // TODO: Get rid of this altogether.
        hwinfo_g->nthreads_hw_avail = hwinfo_g->hardwareTopology->machine().logicalProcessorCount;

        /* detect GPUs */
        hwinfo_g->gpu_info.n_dev            = 0;
        hwinfo_g->gpu_info.n_dev_compatible = 0;
        hwinfo_g->gpu_info.gpu_dev          = nullptr;

        /* Run the detection if the binary was compiled with GPU support
         * and we requested detection.
         */
        hwinfo_g->gpu_info.bDetectGPUs =
            (bGPUBinary && bDetectGPUs &&
             getenv("GMX_DISABLE_GPU_DETECTION") == nullptr);
        if (hwinfo_g->gpu_info.bDetectGPUs)
        {
            gmx_detect_gpus(mdlog, cr);
        }

        gmx_collect_hardware_mpi(*hwinfo_g->cpuInfo);
    }
    /* increase the reference counter */
    n_hwinfo++;

    ret = tMPI_Thread_mutex_unlock(&hw_info_lock);
    if (ret != 0)
    {
        gmx_fatal(FARGS, "Error unlocking hwinfo mutex: %s", strerror(errno));
    }

    return hwinfo_g;
}

bool compatibleGpusFound(const gmx_gpu_info_t &gpu_info)
{
    return gpu_info.n_dev_compatible > 0;
}

void gmx_hardware_info_free(gmx_hw_info_t *hwinfo)
{
    int ret;

    ret = tMPI_Thread_mutex_lock(&hw_info_lock);
    if (ret != 0)
    {
        gmx_fatal(FARGS, "Error locking hwinfo mutex: %s", strerror(errno));
    }

    /* decrease the reference counter */
    n_hwinfo--;


    if (hwinfo != hwinfo_g)
    {
        gmx_incons("hwinfo < hwinfo_g");
    }

    if (n_hwinfo < 0)
    {
        gmx_incons("n_hwinfo < 0");
    }

    if (n_hwinfo == 0)
    {
        delete hwinfo_g->cpuInfo;
        delete hwinfo_g->hardwareTopology;
        free_gpu_info(&hwinfo_g->gpu_info);
        sfree(hwinfo_g);
    }

    ret = tMPI_Thread_mutex_unlock(&hw_info_lock);
    if (ret != 0)
    {
        gmx_fatal(FARGS, "Error unlocking hwinfo mutex: %s", strerror(errno));
    }
}
