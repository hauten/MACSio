/*
Copyright (c) 2015, Lawrence Livermore National Security, LLC.
Produced at the Lawrence Livermore National Laboratory.
Written by Mark C. Miller

LLNL-CODE-676051. All rights reserved.

This file is part of MACSio

Please also read the LICENSE file at the top of the source code directory or
folder hierarchy.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License (as published by the Free Software
Foundation) version 2, dated June 1991.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the terms and conditions of the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <stdlib.h>

#ifdef HAVE_SCR
#ifdef __cplusplus
extern "C" {
#endif
#include <scr.h>
#ifdef __cplusplus
}
#endif
#endif

#include <macsio_mif.h>

#define MACSIO_MIF_BATON_OK  0
#define MACSIO_MIF_BATON_ERR 1
#define MACSIO_MIF_MIFMAX -1
#define MACSIO_MIF_MIFAUTO -2

/*!
\addtogroup MACSIO_MIF
@{
*/

typedef struct _MACSIO_MIF_baton_t
{
    MACSIO_MIF_ioFlags_t ioFlags; /**< Various flags controlling behavior. */
#ifdef HAVE_MPI
    MPI_Comm mpiComm;           /**< The MPI communicator being used */
#endif
    int commSize;               /**< The size of the MPI comm */
    int rankInComm;             /**< Rank of this processor in the MPI comm */
    int numGroups;              /**< Number of groups the MPI comm is divided into */
    int numGroupsWithExtraProc; /**< Number of groups that contain one extra proc/rank */
    int groupSize;              /**< Nominal size of each group (some groups have one extra) */
    int groupRank;              /**< Rank of this processor's group */
    int commSplit;              /**< Rank of the last MPI task assigned to +1 groups */
    int rankInGroup;            /**< Rank of this processor within its group */
    int procBeforeMe;           /**< Rank of processor before this processor in the group */
    int procAfterMe;            /**< Rank of processor after this processor in the group */
    mutable int mifErr;         /**< MIF error value */
    mutable int mpiErr;         /**< MPI error value */
    int mpiTag;                 /**< MPI message tag used for all messages here */
    MACSIO_MIF_CreateCB createCb; /**< Create file callback */
    MACSIO_MIF_OpenCB openCb;   /**< Open file callback */
    MACSIO_MIF_CloseCB closeCb; /**< Close file callback */
    void *clientData;           /**< Client data to be passed around in calls */
} MACSIO_MIF_baton_t;

/*!
\brief Initialize MACSIO_MIF for a MIF I/O operation

Creates and returns a MACSIO_MIF \em baton object establishing the mapping
between MPI ranks and file groups for a MIF I/O operation.

All processors in the \c mpiComm communicator must call this function
collectively with identical values for \c numFiles, \c ioFlags, and \c mpiTag.

The resultant \em baton object is used in subsequent calls to WaitFor and
HandOff the baton to the next processor in each group.

The \c createCb, \c openCb, \c closeCb callback functions are used by MACSIO_MIF
to execute baton waits and handoffs during which time a group's file will be
closed by the HandOff function and opened by the WaitFor method except for the
first processor in each group which will create the file.

Processors in the \c mpiComm communicator are broken into \c numFiles groups.
If there is a remainder, \em R, after dividing the communicator size into
\c numFiles groups, then the first \em R groups will have one additional
processor.

\returns The MACSIO_MIF \em baton object
*/
//#warning ADD A THROTTLE OPTION HERE FOR TOT FILES VS CONCURRENT FILES
//#warning FOR AUTO MODE, MUST HAVE A CALL TO QUERY FILE COUNT
MACSIO_MIF_baton_t *MACSIO_MIF_Init(
    int numFiles,                   /**< [in] Number of resultant files. Note: this is entirely independent of
                                         number of processors. Typically, this number is chosen to match
                                         the number of independent I/O pathways between the nodes the
                                         application is executing on and the filesystem. Pass MACSIO_MIF_MAX for
                                         file-per-processor. Pass MACSIO_MIF_AUTO (currently not supported) to
                                         request that MACSIO_MIF determine and use an optimum file count. */
    MACSIO_MIF_ioFlags_t ioFlags,   /**< [in] See MACSIO_MIF_ioFlags_t for meaning of flags. */
#ifdef HAVE_MPI
    MPI_Comm mpiComm,               /**< [in] The MPI communicator containing all the MPI ranks that will
                                         marshall data in the MIF I/O operation. */
#else
    int      mpiComm,               /**< [in] Dummy arg (ignored) for MPI communicator */
#endif
    int mpiTag,                     /**< [in] MPI message tag MACSIO_MIF will use in all MPI messages for
                                         this MIF I/O operation. */
    MACSIO_MIF_CreateCB createCb,   /**< [in] Callback MACSIO_MIF should use to create a group's file */
    MACSIO_MIF_OpenCB openCb,       /**< [in] Callback MACSIO_MIF should use to open a group's file */
    MACSIO_MIF_CloseCB closeCb,     /**< [in] Callback MACSIO_MIF should use to close a group's file */
    void *clientData                /**< [in] Optional, client specific data MACSIO_MIF will pass to callbacks */
)
{
    int numGroups = numFiles;
    int commSize, rankInComm;
    int groupSize, numGroupsWithExtraProc, commSplit,
        groupRank, rankInGroup, procBeforeMe, procAfterMe;
    MACSIO_MIF_baton_t *ret = 0;

    procBeforeMe = -1;
    procAfterMe = -1;

    MPI_Comm_size(mpiComm, &commSize);
    MPI_Comm_rank(mpiComm, &rankInComm);

    groupSize              = commSize / numGroups;
    numGroupsWithExtraProc = commSize % numGroups;
    commSplit = numGroupsWithExtraProc * (groupSize + 1);

    if (rankInComm < commSplit)
    {
        groupRank = rankInComm / (groupSize + 1);
        rankInGroup = rankInComm % (groupSize + 1);
        if (rankInGroup < groupSize)
            procAfterMe = rankInComm + 1;
    }
    else
    {
        groupRank = numGroupsWithExtraProc + (rankInComm - commSplit) / groupSize; 
        rankInGroup = (rankInComm - commSplit) % groupSize;
        if (rankInGroup < groupSize - 1)
            procAfterMe = rankInComm + 1;
    }
    if (rankInGroup > 0)
        procBeforeMe = rankInComm - 1;

    if (createCb == 0 || openCb == 0 || closeCb == 0)
        return 0;

    ret = (MACSIO_MIF_baton_t *) malloc(sizeof(MACSIO_MIF_baton_t));
    ret->ioFlags = ioFlags;
    ret->commSize = commSize;
    ret->rankInComm = rankInComm;
    ret->numGroups = numGroups;
    ret->groupSize = groupSize;
    ret->numGroupsWithExtraProc = numGroupsWithExtraProc;
    ret->commSplit = commSplit;
    ret->groupRank = groupRank;
    ret->rankInGroup = rankInGroup;
    ret->procBeforeMe = procBeforeMe;
    ret->procAfterMe = procAfterMe;
    ret->mifErr = MACSIO_MIF_BATON_OK;
#ifdef HAVE_MPI
    ret->mpiErr = MPI_SUCCESS;
#else
    ret->mpiErr = 0;
#endif
    ret->mpiTag = mpiTag;
    ret->mpiComm = mpiComm;
    ret->createCb = createCb;
    ret->openCb = openCb;
    ret->closeCb = closeCb;
    ret->clientData = clientData;

    return ret;
}

/*!
\brief End a MACSIO_MIF I/O operation and free resources
*/
void MACSIO_MIF_Finish(
    MACSIO_MIF_baton_t *bat /**< [in] The MACSIO_MIF baton handle */
)
{
    free(bat);
}

/*!
\brief Wait for exclusive access to the group's file

All processors call this function collectively. For the first processor in each
group, this call returns immediately. For all others in the group, it blocks,
waiting for the processor \em before it to finish its work on the group's file
and call the HandOff function.

\returns A void pointer to whatever data instance the \c createCb or \c openCb
methods return. The caller must cast this returned pointer to the correct type.

*/
void * MACSIO_MIF_WaitForBaton(
    MACSIO_MIF_baton_t *Bat, /**< [in] The MACSIO_MIF baton handle */
    char const *fname,       /**< [in] The filename */
    char const *nsname       /**< [in] The namespace within the file to be used for objects in this code block.  */
)
{
    if (Bat->procBeforeMe != -1)
    {
        MPI_Status mpi_stat;
        int baton;
        int mpi_err = MPI_Recv(&baton, 1, MPI_INT, Bat->procBeforeMe,
            Bat->mpiTag, Bat->mpiComm, &mpi_stat);
        if (mpi_err == MPI_SUCCESS && baton != MACSIO_MIF_BATON_ERR)
        {
#ifdef HAVE_SCR
            char scr_filename[SCR_MAX_FILENAME];
            if (Bat->ioFlags.use_scr)
            {
                if (SCR_Route_file(fname, scr_filename) == SCR_SUCCESS)
                    return Bat->openCb(scr_filename, nsname, Bat->ioFlags, Bat->clientData);
                else
                    return Bat->openCb(fname, nsname, Bat->ioFlags, Bat->clientData);
            }
            else
#endif
                return Bat->openCb(fname, nsname, Bat->ioFlags, Bat->clientData);
        }
        else
        {
            Bat->mifErr = MACSIO_MIF_BATON_ERR;
            Bat->mpiErr = mpi_err;
            return 0;
        }
    }
    else
    {
        if (Bat->ioFlags.do_wr)
        {
#ifdef HAVE_SCR
            char scr_filename[SCR_MAX_FILENAME];
            if (Bat->ioFlags.use_scr)
            {
                SCR_Route_file(fname, scr_filename);
                return Bat->createCb(scr_filename, nsname, Bat->clientData);
            }
            else
#endif
                return Bat->createCb(fname, nsname, Bat->clientData);
        }
        else
        {
#ifdef HAVE_SCR
            char scr_filename[SCR_MAX_FILENAME];
            if (Bat->ioFlags.use_scr)
            {
                if (SCR_Route_file(fname, scr_filename) == SCR_SUCCESS)
                    return Bat->openCb(scr_filename, nsname, Bat->ioFlags, Bat->clientData);
                else
                    return Bat->openCb(fname, nsname, Bat->ioFlags, Bat->clientData);
            }
            else
#endif
                return Bat->openCb(fname, nsname, Bat->ioFlags, Bat->clientData);
        }
    }
}

/*!
\brief Release exclusive access to the group's file

This function closes the group's file for this processor and hands off control
to the next processor in the group.

*/
void MACSIO_MIF_HandOffBaton(
    MACSIO_MIF_baton_t const *Bat, /**< [in] The MACSIO_MIF baton handle */ 
    void *file                     /**< [in] A void pointer to the group's file handle */
)
{
    Bat->closeCb(file, Bat->clientData);
    if (Bat->procAfterMe != -1)
    {
        int baton = Bat->mifErr;
        int mpi_err = MPI_Ssend(&baton, 1, MPI_INT, Bat->procAfterMe,
            Bat->mpiTag, Bat->mpiComm);
        if (mpi_err != MPI_SUCCESS)
        {
            Bat->mifErr = MACSIO_MIF_BATON_ERR;
            Bat->mpiErr = mpi_err;
        }
    }
}

/*!
\brief Rank of the group in which a given (global) rank exists.

Given the rank of a processor in \c mpiComm used in the MACSIO_MIF_Init()
call, this function returns the rank of the \em group in which the given
processor exists. This function can be called from any rank and will
return correct values for any rank it is passed.
*/
int MACSIO_MIF_RankOfGroup(
    MACSIO_MIF_baton_t const *Bat, /**< [in] The MACSIO_MIF baton handle */
    int rankInComm                 /**< [in] The (global) rank of a proccesor for which rank in group is desired */
)
{
    int retval;

    if (rankInComm < Bat->commSplit)
    {
        retval = rankInComm / (Bat->groupSize + 1);
    }
    else
    {
        retval = Bat->numGroupsWithExtraProc +
                 (rankInComm - Bat->commSplit) / Bat->groupSize; 
    }

    return retval;
}

/*!
\brief Rank within a group of a given (global) rank

Given the rank of a processor in \c mpiComm used in the MACSIO_MIF_Init()
call, this function returns its rank within its group. This function can
be called from any rank and will return correct values for any rank it is
passed.
*/
int MACSIO_MIF_RankInGroup(
    MACSIO_MIF_baton_t const *Bat, /**< [in] The MACSIO_MIF baton handle */
    int rankInComm                 /**< [in] The (global) rank of a processor for which the rank within it's group is desired */
)
{
    int retval;

    if (rankInComm < Bat->commSplit)
    {
        retval = rankInComm % (Bat->groupSize + 1);
    }
    else
    {
        retval = (rankInComm - Bat->commSplit) % Bat->groupSize;
    }

    return retval;
}

/*!@}*/
