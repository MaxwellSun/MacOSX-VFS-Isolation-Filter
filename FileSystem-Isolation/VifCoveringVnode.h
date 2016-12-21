/* 
 * Copyright (c) 2011 Slava Imameev. All rights reserved.
 */

#ifndef _VIFCOVERINGVNODE_H
#define _VIFCOVERINGVNODE_H

#include <libkern/c++/OSObject.h>
#include <IOKit/assert.h>
#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSArray.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/vnode.h>

#include "Common.h"
#include "FltFakeFSD.h"
#include "VifIOVnode.h"
#include "VifSparseFile.h"


//--------------------------------------------------------------------

//
// a helper class as it is tricky to pass structure as a template's argument
//
/*
 class vnop_open_args_class{
 
 public:
 vnop_open_args_class( vnop_open_args** ptr) : ap(ptr) {;}
 vnop_open_args** ap;
 };
 */

#define DECLARE_VNOP_ARG_WRAPPER_CLASS( ARG )\
class ARG##_class{\
public:\
ARG##_class( ARG* ptr) : ap(ptr) {;}\
ARG* ap;\
};

DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_open_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_close_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_access_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_inactive_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_reclaim_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_read_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_write_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_pagein_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_pageout_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_strategy_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_advlock_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_allocate_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_blktooff_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_blockmap_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_bwrite_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_copyfile_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_exchange_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_fsync_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_getattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_getxattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_ioctl_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_link_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_listxattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_kqfilt_add_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_kqfilt_remove_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_mkdir_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_mknod_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_mmap_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_mnomap_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_offtoblk_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_pathconf_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_readdir_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_readdirattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_readlink_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_remove_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_removexattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_revoke_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_rename_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_rmdir_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_searchfs_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_select_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_setattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_setxattr_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_symlink_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_whiteout_args )

#ifdef __APPLE_API_UNSTABLE
#if NAMEDSTREAMS
// TO DO - the following three definitions are from Apple unstable kernel
// portion and requires name streams to be compiled in the kernel,
// might prevent the driver from loading
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_getnamedstream_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_makenamedstream_args )
DECLARE_VNOP_ARG_WRAPPER_CLASS( vnop_removenamedstream_args )
#endif // __APPLE_API_UNSTABLE
#endif // NAMEDSTREAMS

#ifdef _VIF_MACOSX_VFS_ISOLATION

//--------------------------------------------------------------------

typedef struct _VifVnodCreateParams{
    
    //
    // a directory, mught be a native vnode or covering vnode
    //
    __in vnode_t dvp;
    
    //
    // a naitive vnode to cover
    //
    __in vnode_t coveredVnode;
    
    //
    // flags, the same as in vnode_fsparam.vnfs_flags
    //
    __in uint32_t vnfs_flags;
    
    //
    // created vnode
    //
    __out vnode_t coveringVnode;
    
} VifVnodCreateParams;

//--------------------------------------------------------------------

class VifCoveredVnode{
    
public:
    
    VifCoveredVnode(): 
        coveredVnode(NULL),
        dldCoveredVnode(NULL),
        coveringVnode(NULL),
        dldCoveringVnode(NULL){ ;}
    
    ~VifCoveredVnode(){ 
        assert( NULL == this->coveringVnode );
        assert( NULL == this->coveredVnode );
        assert( NULL == this->dldCoveredVnode );
        assert( NULL == this->dldCoveringVnode );
    }
    
    errno_t getVnodeInfo( __in vnode_t coveringVnode );
    void putVnodeInfo();
    
    vnode_t         coveredVnode; // referenced
    VifIOVnode*     dldCoveredVnode; // referenced
    
    vnode_t         coveringVnode; // not referenced!
    VifIOVnode*     dldCoveringVnode; // referenced
    
};

//--------------------------------------------------------------------

typedef struct _VifCoveringFsdResult{
    
    kern_return_t     status;
    
    struct{
        unsigned int passThrough:0x1; // call an underlying FSD
        unsigned int completeWithStatus:0x1; // complete with the returned status 
    } flags;
    
} VifCoveringFsdResult;

//--------------------------------------------------------------------

typedef struct _VifCfsdIOArgs{
    
#if defined( DBG )
#define VIF_CFSD_WRVN_SIGNATURE   0xABCD32FF
    UInt32         signature;
#endif//DBG
    
    //
    // in case of the pageout V2 the upl is created by FSD, so we
    // create our own upl and free it on completion
    //
    upl_t          upl;
    
    //
    // an array of the IOMemoryDescriptor* for ap.a_uio vectors,
    // released when it is no longer needed by a recepient,
    // each entry describes exactly a single contiguous range
    // of memory
    //
    OSArray*       memDscArray;
    
    //
    // an array for the VifRangeDescriptors related with
    // the memory descriptors in the memDscArray, saved
    // as OSData binary objects
    //
    OSArray*      rangeDscArray;
    
    //
    // flags for IO
    //
    int           ioflag;
    
    //
    // tracks the data left to process
    //
    user_ssize_t  residual;
    
    struct {
        unsigned int   pagingIO:0x1;
        unsigned int   read:0x1;
        unsigned int   write:0x1;
    } flags;
    
    //
    // a referenced IO context, might be NULL
    //
    vfs_context_t contextRef;
    
} VifCfsdIOArgs;

//--------------------------------------------------------------------

typedef struct _VifPagingIoArgs{
    
    vnode_t         a_vp;// this is a covered vnode found by getInputArguments, be vigilant!
    upl_t           a_pl;
    upl_offset_t    a_pl_offset;
    off_t           a_f_offset;
    size_t          a_size;
    int             a_flags;
    vfs_context_t   a_context;
    
    struct{
        unsigned int read:1;
        unsigned int write:1;
    } flags;
    
} VifPagingIoArgs;


class VifCoveringFsd : public OSObject
{
    OSDeclareDefaultStructors( VifCoveringFsd )
    
private:
    
    IORecursiveLock*   lock;
    
    //
    // a mnt structure for a covered FSD,
    // if NULL this is a global covering FSD
    //
    __opt mount_t      mnt;
    
    errno_t VifFsdHookCreateCoveringVnodeInternal( __inout VifVnodCreateParams*  ap );
    
    void processPagingIO( __in VifCoveredVnode* coverdVnode,
                          __in VifPagingIoArgs* ap,
                          __inout VifCoveringFsdResult* result );
    
    static VifCoveringFsd*   GlobalCoveringFsd;
    
private:
    
    //
    // read/write data from/to covering vnode, if required the covered vnode may be accessed
    //
    kern_return_t rwData( __in VifCoveredVnode*  coverdVnode, __in VifCfsdIOArgs* args );
    
protected:
    
    virtual bool init();
    
    /*! @function free
     @abstract Frees data structures that were allocated by init()*/
    
    virtual void free( void );

    
public:
    
    static VifCoveringFsd* withMount( __in_opt mount_t mnt );
    
    bool VifIsCoveringVnode( __in vnode_t vnode );
    
    //
    // returns the covered vnode for covering one
    //
    vnode_t VifGetCoveredVnode( __in vnode_t coveringVnode );
    
    //
    // set the UBC file size for the covering vnode
    //
    void VifSetUbcFileSize( __in VifCoveredVnode*  vnodesDscr,
                            __in off_t   size,
                            __in bool    forceSetSize );
    
    //
    // gets a covered vnode and replaces it with the covering one
    //
    int VifReplaceVnodeByCovering( __inout vnode_t* vnodeToCover,
                                   __in    vnode_t dvp );
    
    int VifReclaimCoveringVnode( __in vnode_t  coveringVnode );
    
    void LockShared();
    void UnLockShared();
    void LockExclusive();
    void UnLockExclusive();
    
    //
    // currently there is a global covering FSD, the every returned
    // object the PutCoveringFsd() should be called
    //
    static bool InitCoveringFsd();
    static VifCoveringFsd* GetCoveringFsd( __in mount_t mnt );
    static void PutCoveringFsd( __in VifCoveringFsd* coveringFsd );
    
    //
    // process pageout request for covering vnode
    //
    void processPageout( __in VifCoveredVnode*  coverdVnode,
                         __in struct vnop_pageout_args *ap,
                         __inout VifCoveringFsdResult* result);
    
    //
    // process pagein fo covering vnode
    //
    void processPagein( __in VifCoveredVnode*  coverdVnode,
                        __in struct vnop_pagein_args *ap,
                        __inout VifCoveringFsdResult* result);
    
    //
    // process buffered write
    //
    void processWrite( __in VifCoveredVnode*  coverdVnode,
                       __in struct vnop_write_args *ap,
                       __inout VifCoveringFsdResult* result);
    
    //
    // process buffered read
    //
    void processRead( __in VifCoveredVnode*  coverdVnode,
                      __in struct vnop_read_args *ap,
                      __inout VifCoveringFsdResult* result);
    
    void releaseIOArgs( __in VifCfsdIOArgs* args);
    
    //
    // returns a referenced vnode for covering vnode by covered file name,
    // a caller must call vnode_put for a returned vnode, NULL can be returned,
    // should not be used frequently as time consuming and prone to deadlocks
    // if called deep on a call stack
    //
    vnode_t getCoveringVnodeRefByCoveredFileName( __in char* coveredFileName );
    
    errno_t createIsolationSparseFile( __in VifCoveredVnode*  coveredVnode, __in vfs_context_t  vfsContext );
};

//--------------------------------------------------------------------

template<class ARG>
class VifVnopArgs
{
public:
    
    VifVnopArgs(): fsd(NULL) { flags.released = 0x0; }
    ~VifVnopArgs(){ assert( NULL == fsd ); }
    
    void putAllVnodes();
    
    errno_t getInputArguments( __inout ARG* apWrapper );
    void    putInputArguments( __inout ARG* apWrapper );
    
    //errno_t getInputArguments( __inout struct vnop_close_args** ap );
    
    //
    // currently two vnodes is enough for any operation we are intercepting
    //
    VifCoveredVnode  vnode1;
    VifCoveredVnode  vnode2;
    VifCoveringFsd*  fsd;
    
    struct{
        unsigned int released:0x1;
    } flags;
    
};

//--------------------------------------------------------------------

template<class ARG>
void
VifVnopArgs<ARG>::putAllVnodes()
{
    this->vnode1.putVnodeInfo();
    this->vnode2.putVnodeInfo();
}

//--------------------------------------------------------------------

//
// putInputArguments template and its specializations must be defined
// before getInputArguments template and specializations as the former
// are called by the latter and GCC compiler must instantiate them
// before the first call and it instantiates functions in order of
// definitions
//

template<class ARG>
void
VifVnopArgs<ARG>::putInputArguments( __inout ARG* apWrapper )
{
    if( 0x1 == this->flags.released )
        return;
    
    assert( NULL == this->vnode1.coveringVnode || apWrapper->ap->a_vp == this->vnode1.coveredVnode );
    
    //
    // restore the original
    //
    if( this->vnode1.coveringVnode )
        apWrapper->ap->a_vp = this->vnode1.coveringVnode;
    
    //
    // release all vnodes
    //
    this->putAllVnodes();
    
    if( this->fsd ){
        
        VifCoveringFsd::PutCoveringFsd( this->fsd );
        this->fsd = NULL;
    }
    
    this->flags.released = 0x1;
}

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_strategy_args_class>::putInputArguments( __inout vnop_strategy_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_bwrite_args_class>::putInputArguments( __inout vnop_bwrite_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_exchange_args_class>::putInputArguments( __inout vnop_exchange_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_copyfile_args_class>::putInputArguments( __inout vnop_copyfile_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_mkdir_args_class>::putInputArguments( __inout vnop_mkdir_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_mknod_args_class>::putInputArguments( __inout vnop_mknod_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_rename_args_class>::putInputArguments( __inout vnop_rename_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_symlink_args_class>::putInputArguments( __inout vnop_symlink_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
void
VifVnopArgs<vnop_whiteout_args_class>::putInputArguments( __inout vnop_whiteout_args_class* apWrapper );

//--------------------------------------------------------------------

template<class ARG>
errno_t
VifVnopArgs<ARG>::getInputArguments( __inout ARG* apWrapper )
{
    
    errno_t   error = KERN_SUCCESS;
    
    assert( apWrapper->ap );
    assert( NULL == this->fsd );
    
    this->fsd = VifCoveringFsd::GetCoveringFsd( vnode_mount( apWrapper->ap->a_vp ) );
    assert( this->fsd );
    if( !this->fsd )
        goto __exit;
    
    error = this->vnode1.getVnodeInfo( apWrapper->ap->a_vp );
    assert( KERN_SUCCESS == error );
    if( KERN_SUCCESS != error )
        goto __exit;
    
    assert( NULL == this->vnode1.coveringVnode || apWrapper->ap->a_vp == this->vnode1.coveringVnode );

    //
    // replace the covering vnode to covered one
    //
    if( this->vnode1.coveredVnode )
        apWrapper->ap->a_vp = this->vnode1.coveredVnode;
    
    this->flags.released = 0x0;
    
__exit:
    
    if( error )
        this->VifVnopArgs<ARG>::putInputArguments( apWrapper );
    
    return error;
}

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_strategy_args_class>::getInputArguments( __inout vnop_strategy_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_bwrite_args_class>::getInputArguments( __inout vnop_bwrite_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_exchange_args_class>::getInputArguments( __inout vnop_exchange_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_copyfile_args_class>::getInputArguments( __inout vnop_copyfile_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_mkdir_args_class>::getInputArguments( __inout vnop_mkdir_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_mknod_args_class>::getInputArguments( __inout vnop_mknod_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_rename_args_class>::getInputArguments( __inout vnop_rename_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_symlink_args_class>::getInputArguments( __inout vnop_symlink_args_class* apWrapper );

//--------------------------------------------------------------------

template<>
errno_t
VifVnopArgs<vnop_whiteout_args_class>::getInputArguments( __inout vnop_whiteout_args_class* apWrapper );

//--------------------------------------------------------------------

#endif //#ifdef _VIF_MACOSX_VFS_ISOLATION
#endif//_VIFCOVERINGVNODE_H
