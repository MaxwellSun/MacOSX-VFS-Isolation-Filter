/*
 * Copyright (c) 2010 Slava Imameev. All rights reserved.
 */

#include <vfs/vfs_support.h>
#include <sys/ubc.h>
#include <sys/buf.h>
#include <sys/mman.h>

#include "VNodeHook.h"
#include "VmPmap.h"
#include "VFSHooks.h"
#include "VifCoveringVnode.h"
#include "VifVnodeHashTable.h"
#include "Kauth.h"
#include "VifVfsMntHook.h"
#include "FltFakeFSD.h"

//--------------------------------------------------------------------

#if defined( DBG )
static SInt32   gVifVNodeHookCount;
#endif//DBG

#ifndef _VIF_MACOSX_VFS_ISOLATION
    #error "Are you sure you want to compile without VFS isolation support?"
#endif //_VIF_MACOSX_VFS_ISOLATION

//--------------------------------------------------------------------

#ifdef _VIF_MACOSX_VFS_ISOLATION

/*
 //--------------------------------------------------------------------
 
 typedef struct _VifCoverVnodeData{
 
 //
 // a naitive vnode covered by this vnode,
 // not retained
 //
 struct vnode* a_vp;
 
 } VifCoverVnodeData;
 
 
 //--------------------------------------------------------------------
 
 //
 // TO DO hash
 //
 LIST_ENTRY  gCnodeListHead;
 IORecursiveLock* gFsdLock;
 
 typedef struct _VifCnode{
 
 LIST_ENTRY  listEntry;
 
 //
 // a vnode for this cnode
 //
 vnode_t    vnode;
 
 //
 // a covered node
 //
 vnode_t    coveredVnode;
 
 } VifCnode;
 
 //--------------------------------------------------------------------
 
 */

#endif//#ifdef _VIF_MACOSX_VFS_ISOLATION

//--------------------------------------------------------------------

void
VifLogVnodeOperation( __in vnode_t vnode, __in VifIOVnode::VnodeOperation op )
{
    
    VifIOVnode*  dldVnode;
    
    dldVnode = VifVnodeHashTable::sVnodesHashTable->RetrieveReferencedIOVnodByBSDVnode( vnode );
    if( dldVnode ){
        
        dldVnode->logVnodeOperation( op );
        dldVnode->release();
        
    }// end if( dldVnode )
}

//--------------------------------------------------------------------

void
VifVnodePrepareAuditDataForReuse( __in vnode_t vnode )
{
    
    VifIOVnode*  dldVnode;
    
    dldVnode = VifVnodeHashTable::sVnodesHashTable->RetrieveReferencedIOVnodByBSDVnode( vnode );
    if( dldVnode ){
        
        dldVnode->prepareAuditDataForReuse();
        dldVnode->release();
        
    }// end if( dldVnode )
}

//--------------------------------------------------------------------

void
VifCheckForCoveredVnodeBreakIf( __in vnode_t vnode )
{
    //
    // check that a vnode for a file on a volume with FAT32
    // file system is covered, I presume that all FAT32 volumes
    // are marked as removable and ISOALTION is on for all users
    //
    if( vnode_isreg( vnode ) ){
        
        struct vfsstatfs  *vfsstat = vfs_statfs( vnode_mount( vnode ) );
        
        if( 0x0 == memcmp( vfsstat->f_fstypename, "msdos", 5 ) ){
            
            if( NULL != vnode_fsnode( vnode ) ){
                
                __asm__ volatile( "int $0x3" );
                
            } // if( NULL != vnode_fsnode( *ap->a_vpp ) )
            
        } // if( 0x0 == mecmp( vfsstat->f_fstypename, "msdos", 5 ) )
        
    } // if( !error && vnode_isreg( *ap->a_vpp ) )
}

//--------------------------------------------------------------------

bool
TestHarness_IsControlledByIsolationFilter( __in const char* pathname );

#ifdef _VIF_MACOSX_VFS_ISOLATION
#if DBG
static int gErrorToTrack = 0xFFFFF;
static int gErrorToSkip = 2;
#endif//DBG
#endif//#ifdef _VIF_MACOSX_VFS_ISOLATION

static int
VifFsdLookupHook(struct vnop_lookup_args *ap)
/*
 struct vnop_lookup_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_dvp;
 struct vnode **a_vpp;
 struct componentname *a_cnp;
 vfs_context_t a_context;
 } *ap)
 */
{
    int error;
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifCoveringFsd*  fsd = VifCoveringFsd::GetCoveringFsd( vnode_mount( ap->a_dvp ) );
    assert( fsd );
    assert( !fsd->VifIsCoveringVnode( ap->a_dvp ) );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    int (*orig)(struct vnop_lookup_args *ap);
    
    orig = (int (*)(struct vnop_lookup_args*))VifGetOriginalVnodeOp( ap->a_dvp, FltVopEnum_lookup );
    assert( orig );
    
    error = orig( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
#if DBG
    if( ( gErrorToTrack == error ) || ( 0xCFFFFF == gErrorToTrack && error && gErrorToSkip != error ) ){
        
        __asm__ volatile( "int $0x3" );
    }
#endif// DBG
    
    if( ! TestHarness_IsControlledByIsolationFilter( ap->a_cnp->cn_pnbuf ) )
        return error;
    
    if( KERN_SUCCESS == error ){
        
        assert( *ap->a_vpp );
        
        bool isolationOn = false;
        
        //
        // check regular vnodes for ISOALTION processing
        //
        if( vnode_isreg( *ap->a_vpp ) ){
            
            VifIOVnode* dldVnode;
            
            //
            // if added at lookup or create the vnode doesn't have the name attached, the name
            // and the parent are updated later by vnode_update_identity(), we only can be sure that when
            // a KAUTH callback is called a vnode has a valid name and parent
            //
            dldVnode = VifVnodeHashTable::sVnodesHashTable->CreateAndAddIOVnodByBSDVnode( *ap->a_vpp );
            assert( dldVnode );
            
            if( dldVnode ){
                
                //
                // check whether the user is under the isolation control
                //
                dldVnode->defineStatusForIsolation( ap->a_context, ap->a_cnp->cn_pnbuf );
                
                isolationOn = (0x1 == dldVnode->flags.isolationOn);
                dldVnode->release();
            } // if( dldVnode )
        } // if( vnode_isreg( *ap->a_vpp ) )
        
        if( isolationOn ){
            
            //
            // we are creating covering vnodes only for regular vnodes
            //
            assert( vnode_isreg( *ap->a_vpp ) );
            
            error = fsd->VifReplaceVnodeByCovering( ap->a_vpp, ap->a_dvp );
            assert( !error );
            if( error ){
                
                vnode_put( *ap->a_vpp );
                *ap->a_vpp = NULL;
                VifCoveringFsd::PutCoveringFsd( fsd );
                return error;
            }
            
            //
            // covering vnode doesn't have any private data
            //
            assert( NULL == vnode_fsnode( *ap->a_vpp ) );
            
        } // if( vnode_isreg( *ap->a_vpp ) )
        
    } // end if( KERN_SUCCESS == error )
    
    VifCoveringFsd::PutCoveringFsd( fsd );
    
    // TEST!!!!
#if defined(DBG)
    if( !error ){
        
        VifCheckForCoveredVnodeBreakIf( *ap->a_vpp );
        
    } // if( !error )
#endif // DBG
    
#endif//#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdCreateHook(struct vnop_create_args *ap)
/*
 struct vnop_create_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_dvp;
 struct vnode **a_vpp;
 struct componentname *a_cnp;
 struct vnode_attr *a_vap;
 vfs_context_t a_context;
 } *ap;
 */
{
    int error;
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifCoveringFsd*  fsd = VifCoveringFsd::GetCoveringFsd( vnode_mount( ap->a_dvp ) );
    assert( fsd );
    assert( !fsd->VifIsCoveringVnode( ap->a_dvp ) );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    int (*orig)(struct vnop_create_args *ap);
    
    orig = (int (*)(struct vnop_create_args*))VifGetOriginalVnodeOp( ap->a_dvp, FltVopEnum_create );
    assert( orig );
    
    error = orig( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( ! TestHarness_IsControlledByIsolationFilter( ap->a_cnp->cn_pnbuf ) )
        return error;
    
    if( KERN_SUCCESS == error ){
        
        bool    isolationOn = false;
        
        assert( *ap->a_vpp );
        
        //
        // check regular vnodes for ISOALTION processing
        //
        if( vnode_isreg( *ap->a_vpp ) ){
            
            VifIOVnode* dldVnode;
            
            //
            // if added at lookup or create the vnode doesn't have the name attached, the name
            // and the parent are updated later by vnode_update_identity(), we only can be sure that when
            // a KAUTH callback is called a vnode has a valid name and parent
            //
            dldVnode = VifVnodeHashTable::sVnodesHashTable->CreateAndAddIOVnodByBSDVnode( *ap->a_vpp );
            assert( dldVnode );
            
            if( dldVnode ){
                
                //
                // check whether the user is under the isolation control
                //
                dldVnode->defineStatusForIsolation( ap->a_context, ap->a_cnp->cn_pnbuf );
                
                isolationOn = (0x1 == dldVnode->flags.isolationOn);
                dldVnode->release();
            } // if( dldVnode )
        } // if( vnode_isreg( *ap->a_vpp ) )
        
        if( isolationOn ){
            
            //
            // we are creating covering vnodes only for regular files
            //
            
            error = fsd->VifReplaceVnodeByCovering( ap->a_vpp, ap->a_dvp );
            assert( !error );
            if( error ){
                
                vnode_put( *ap->a_vpp );
                *ap->a_vpp = NULL;
                VifCoveringFsd::PutCoveringFsd( fsd );
                return error;
            }
            
            //
            // covering vnode doesn't have any private data
            //
            assert( NULL == vnode_fsnode( *ap->a_vpp ) );
            
        } // if( vnode_isreg( *ap->a_vpp ) )
        
    } // end if( KERN_SUCCESS == error )
    
    VifCoveringFsd::PutCoveringFsd( fsd );
    
    // TEST!!!!
#if defined(DBG)
    if( !error ){
        
        VifCheckForCoveredVnodeBreakIf( *ap->a_vpp );
        
    } // if( !error )
#endif // DBG
    
#endif//#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdOpenHook(struct vnop_open_args *ap)
/*
 struct vnop_open_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 int a_mode;
 vfs_context_t a_context;
 } *ap;
 */
{
    int                  error;
    vnop_open_args_class apc( ap );
    
    VifLogVnodeOperation( ap->a_vp, VifIOVnode::kVnodeOp_Open );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_open_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_open_args *ap);
    
    orig = (int (*)(struct vnop_open_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_open );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    if( 0x0 == error ){
        
        //
        // we can't rely on kauth callbacks for close notification as they are skipped
        // when the process is being terminated and proc_exit() calls fdfree() that calls
        // closef_locked() that does not calls kauth callbacks, so we need to notify about
        // open from the hook to keep the balance between open and close notifications
        //
        
    }
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdCloseHook(struct vnop_close_args *ap)
/*
 struct vnop_close_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 int  a_fflag;
 vfs_context_t a_context;
 } *ap;
 */
/*
 FYI - one of the unusual ways for close is through a revoke call
 #6  0x002fb166 in VNOP_CLOSE (vp=0x8cc48ac, fflag=48, ctx=0x7aad104) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/kpi_vfs.c:3185
 #7  0x002dc29b in vclean (vp=0x8cc48ac, flags=1) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/vfs_subr.c:2010
 #8  0x002dc519 in vgone [inlined] () at :2203
 #9  0x002dc519 in vnode_reclaim_internal (vp=0x8cc48ac, locked=0, reuse=0, flags=9) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/vfs_subr.c:4026
 #10 0x002ded22 in vn_revoke (vp=0x8cc48ac, flags=1, a_context=0x7aad104) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/vfs_subr.c:2145
 #11 0x002ee179 in nop_revoke (ap=0x31c7bce0) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/vfs_support.c:334
 #12 0x002f63d5 in VNOP_REVOKE (vp=0x8cc48ac, flags=1, ctx=0x7aad104) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/kpi_vfs.c:3791
 #13 0x002e3389 in revoke (p=0x67ac540, uap=0xc8f2188, retval=0x7aad044) at /SourceCache/xnu/xnu-1504.7.4/bsd/vfs/vfs_syscalls.c:6717
 #14 0x004edaf8 in unix_syscall64 (state=0xc8f2184) at /SourceCache/xnu/xnu-1504.7.4/bsd/dev/i386/systemcalls.c:433
 */
{
    int                     error;
    vnop_close_args_class   apc( ap );
    
    //
    // we can't rely on kauth callbacks for close notification as they are skipped
    // when the process is being terminated and proc_exit() calls fdfree() that calls
    // closef_locked() that does not calls kauth callbacks,
    // FYI the process is terminated by AST set from the exit1() syscall
    /*
     #0  install_special_handler_locked (thread=0x75ba000) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/thread_act.c:664
     #1  0x0022e94e in install_special_handler (thread=0x75ba000) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/thread_act.c:644
     #2  0x0022ee6e in thread_hold (thread=0x75ba000) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/thread_act.c:186
     #3  0x0022c140 in task_hold_locked (task=0x71163e8) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/task.c:893
     #4  0x0022cc82 in get_active_thread [inlined] () at :1163
     #5  0x0022cc82 in task_wait_locked [inlined] () at :947
     #6  0x0022cc82 in task_suspend (task=0x71163e8) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/task.c:1164
     #7  0x00482501 in sig_lock_to_exit (p=0x72d62a0) at /SourceCache/xnu/xnu-1504.7.4/bsd/kern/kern_sig.c:3149
     #8  0x0047733d in exit1 (p=0x72d62a0, rv=0, retval=0x77f83a4) at /SourceCache/xnu/xnu-1504.7.4/bsd/kern/kern_exit.c:268
     #9  0x00477402 in exit (p=0x72d62a0, uap=0x8002cc8, retval=0x77f83a4) at /SourceCache/xnu/xnu-1504.7.4/bsd/kern/kern_exit.c:203
     #10 0x004edaf8 in unix_syscall64 (state=0x8002cc4) at /SourceCache/xnu/xnu-1504.7.4/bsd/dev/i386/systemcalls.c:433
     */
    // the AST terminates te last thread that results in a call to proc_exit()
    /*
     #0  proc_exit (p=0x6789000) at /SourceCache/xnu/xnu-1504.7.4/bsd/kern/kern_exit.c:365
     #1  0x0022e68d in thread_terminate_self () at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/thread.c:344
     #2  0x0022eab9 in special_handler (rh=0x7117a88, thread=0x71177a8) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/thread_act.c:812
     #3  0x0022f4c4 in act_execute_returnhandlers () at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/thread_act.c:722
     #4  0x0021905d in ast_taken (reasons=33, enable=1) at /SourceCache/xnu/xnu-1504.7.4/osfmk/kern/ast.c:168
     #5  0x002a79f7 in i386_astintr (preemption=0) at /SourceCache/xnu/xnu-1504.7.4/osfmk/i386/trap.c:1514
     */
    //
    
    VifLogVnodeOperation( ap->a_vp, VifIOVnode::kVnodeOp_Close );
    
    //
    // the special file's vnodes are normally not reclaimed instead they are reused, so the information
    // must be updated for each KAUTH open callback invokation, TO DO - ponder doing this for every vnode
    //
    if( VCHR == vnode_vtype( ap->a_vp ) || VBLK == vnode_vtype( ap->a_vp ) ){
        
        VifVnodePrepareAuditDataForReuse( ap->a_vp );
    }
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_close_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        vnode_t coveringVnode = vnopArgs.vnode1.coveringVnode;
        assert( vnopArgs.vnode1.coveredVnode );
        
        //
        // flush the cache
        // TO DO this is to avoid a deadlock when the system waits forever
        // on unmount during the shutdown because of dirty pages, the reason
        // must be investigated and fixed dicently
        //
        cluster_push( coveringVnode, IO_CLOSE );
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_close_args *ap);
    
    orig = (int (*)(struct vnop_close_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_close );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdAccessHook(struct vnop_access_args *ap)
/*
 struct vnop_access_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 int a_action;
 vfs_context_t a_context;
 } *ap;
 */
{
    int         error;
    vnop_access_args_class  apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_access_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_access_args *ap);
    
    orig = (int (*)(struct vnop_access_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_access );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdInactiveHook(struct vnop_inactive_args *ap)
/*
 struct vnop_inactive_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 vfs_context_t a_context;
 } *ap;
 */
{
    int         error;
    vnop_inactive_args_class  apc( ap );
    
    VifLogVnodeOperation( ap->a_vp, VifIOVnode::kVnodeOp_Inactive );
    
    //
    // prepare the object for reuse
    //
    VifVnodePrepareAuditDataForReuse( ap->a_vp );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_inactive_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        vnode_t coveringVnode = vnopArgs.vnode1.coveringVnode;
        assert( vnopArgs.vnode1.coveredVnode );
        
        //
        // flush the cache
        // TO DO this is to avoid a deadlock when the system waits forever
        // on unmount during the shutdown because of dirty pages, the reason
        // must be investigated and fixed dicently
        //
        cluster_push( coveringVnode, IO_CLOSE );
        
        //
        // recycle the covering vnode ( like the union FSD does )
        //
        vnode_recycle( coveringVnode );
        
        vnopArgs.putInputArguments( &apc );
        
        return KERN_SUCCESS;
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_inactive_args *ap);
    
    orig = (int (*)(struct vnop_inactive_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_inactive );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

/*
 just FYI , one of the ways when reclaimed is called
 
 #3  0x46d00821 in VifFsdReclaimHook (ap=0x31adbcd4) at /work/DeviceLockProject/DeviceLockIOKitDriver/VifVNodeHook.cpp:809
 #4  0x00329f8d in VNOP_RECLAIM (vp=0xbba13e8, ctx=0x7188514) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/kpi_vfs.c:5141
 #5  0x0030ffa1 in vclean (vp=0xbba13e8, flags=0) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_subr.c:2067
 #6  0x00310100 in vgone [inlined] () at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_subr.c:2203
 #7  0x00310100 in vnode_reclaim_internal (vp=0xbba13e8, locked=1, reuse=1, flags=8) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_subr.c:4026
 #8  0x003103c1 in vnode_put_locked (vp=0xbba13e8) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_subr.c:3811
 #9  0x003103f7 in vnode_put (vp=0xbba13e8) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_subr.c:3766
 #10 0x0032467f in vn_closefile (fg=0x62f4000, ctx=0x31adbe88) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_vnops.c:1225
 #11 0x004c4359 in fo_close [inlined] () at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/kern_descrip.c:4869
 #12 0x004c4359 in closef_finish [inlined] () at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/kern_descrip.c:4068
 #13 0x004c4359 in closef_locked (fp=0x5cf3710, fg=0x62f4000, p=0x6bdd2c0) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/kern_descrip.c:4167
 #14 0x004c610a in close_internal_locked (p=0x6bdd2c0, fd=5, fp=0x5cf3710, flags=<value temporarily unavailable, due to optimizations>) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/kern_descrip.c:1950
 #15 0x004c61db in close_nocancel (p=0x6bdd2c0, uap=0x726b9f8, retval=0x7188454) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/kern_descrip.c:1852
 #16 0x0054e9fd in unix_syscall64 (state=0x726b9f4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/dev/i386/systemcalls.c:365
 */

static int
VifFsdReclaimHook(struct vnop_reclaim_args *ap)
/*
 struct vnop_reclaim_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 vfs_context_t a_context;
 } *ap;
 */
/*
 called from the vnode_reclaim_internal() function
 vnode_reclaim_internal()->vgone()->vclean()->VNOP_RECLAIM()
 */
{
    
    errno_t     error;
    vnop_reclaim_args_class  apc( ap );
    VifIOVnode* dldVnode;
    
    dldVnode = VifVnodeHashTable::sVnodesHashTable->RetrieveReferencedIOVnodByBSDVnode( ap->a_vp );
    if( dldVnode ){
        
        dldVnode->logVnodeOperation( VifIOVnode::kVnodeOp_Reclaim );
        dldVnode->prepareForReclaiming();

        dldVnode->release();
        dldVnode = NULL;
        
    } // end if( dldVnode )
    
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifCoveringFsd*  fsd = VifCoveringFsd::GetCoveringFsd( vnode_mount( apc.ap->a_vp ) );
    assert( fsd );
    
    if( fsd->VifIsCoveringVnode( apc.ap->a_vp ) ){
        
        //
        // disregard the return value as the kernel panics on error
        //
        fsd->VifReclaimCoveringVnode( apc.ap->a_vp );
        
        VifCoveringFsd::PutCoveringFsd( fsd );
        
        return KERN_SUCCESS;
    } // end if( fsd->VifIsCoveringVnode() )
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    //
    // check that v_data is not NULL, this is a sanity check to be sure we don't damage FSD structures,
    // some FSDs set v_data to NULL intentionally, for example see the msdosfs's msdosfs_check_link()
    // that calls vnode_clearfsnode() for a temporary vnode of VNON type
    //
    assert( !( VNON != vnode_vtype( apc.ap->a_vp ) && NULL == vnode_fsnode( apc.ap->a_vp ) ) );
    
    int (*orig)(struct vnop_reclaim_args *ap);
    
    orig = (int (*)(struct vnop_reclaim_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_reclaim );
    assert( orig );
    
    //
    // remove the corresponding VifIOVnode
    //
    VifVnodeHashTable::sVnodesHashTable->RemoveIOVnodeByBSDVnode( apc.ap->a_vp );
    
    error = orig( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    VifCoveringFsd::PutCoveringFsd( fsd );
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdReadHook(struct vnop_read_args *ap)
/*
 struct vnop_read_args {
 struct vnodeop_desc *a_desc,
 struct vnode *a_vp;
 struct uio *a_uio;
 int  a_ioflag;
 vfs_context_t a_context;
 } *ap;
 */
{
    
    int                   error;
    vnop_read_args_class  apc( ap );
    bool                  callOriginal = true;
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_read_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
    //
    // process the request to the covering vnode
    //
    if( vnopArgs.vnode1.coveringVnode ){
        
        VifCoveringFsdResult  opResult = {0x0};
        
        assert( vnopArgs.vnode1.coveredVnode );
        
        vnopArgs.fsd->processRead( &vnopArgs.vnode1,
                                  apc.ap,
                                  &opResult );
        
        callOriginal = ( 0x1 == opResult.flags.passThrough );
        
        if( !callOriginal ){
            
            assert( 0x1 == opResult.flags.completeWithStatus );
            error = opResult.status;
        }
        
    } // end if( vnopArgs.vnode1.coveringVnode )
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    if( callOriginal ){
        
        assert( vnode_fsnode( apc.ap->a_vp ) );
        
        int (*orig)(struct vnop_read_args *ap);
        
        orig = (int (*)(struct vnop_read_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_read );
        assert( orig );
        
        error = orig( apc.ap );
        
    } // if( callOriginal )
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        /*
         * XXX
         * perhaps the size of the underlying object has changed under
         * our feet.  take advantage of the offset information present
         * in the uio structure.
         */
        if( !error ){
            
            off_t cur = uio_offset( apc.ap->a_uio );
            
            vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode1, cur, false );
            
        }// end if( !error )
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdWriteHook(struct vnop_write_args *ap)
/*
 struct vnop_write_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 struct uio *a_uio;
 int  a_ioflag;
 vfs_context_t a_context;
 } *ap;
 */
{
    errno_t      error;
    bool         callOriginal = true;
    user_ssize_t bytesToWrite;
    
    vnop_write_args_class  apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_write_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        callOriginal = false;
    }
    
    //
    // check that the covered vnode is valid
    //
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    //
    // process the request to the covering vnode
    //
    if( callOriginal && vnopArgs.vnode1.dldCoveringVnode && vnopArgs.vnode1.dldCoveringVnode->isControlledByIsolationFilter() ){
        
        VifCoveringFsdResult  opResult = {0x0};
        
        assert( vnopArgs.vnode1.coveredVnode );
        
        vnopArgs.fsd->processWrite( &vnopArgs.vnode1,
                                   apc.ap,
                                   &opResult );
        
        callOriginal = ( 0x1 == opResult.flags.passThrough );
        
        if( !callOriginal ){
            
            assert( 0x1 == opResult.flags.completeWithStatus );
            error = opResult.status;
        }
        
    } // if( vnopArgs.vnode1.coveringVnode )
#endif // #ifdef _VIF_MACOSX_VFS_ISOLATION
    
    
    if( callOriginal ){
        
        int (*orig)(struct vnop_write_args *ap);
        
        orig = (int (*)(struct vnop_write_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_write );
        assert( orig );
        
        //
        // call the original, the shadowing will be performed concurrently
        //
        error = orig( apc.ap );
        
    }// end if( callOriginal )
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        /*
         * XXX
         * perhaps the size of the underlying object has changed under
         * our feet.  take advantage of the offset information present
         * in the uio structure.
         */
        if( !error ){
            
            off_t cur = uio_offset( apc.ap->a_uio );
            
            vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode1, cur, false );
            
        }// end if( !error )
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdPageinHook(struct vnop_pagein_args *ap)
/*
 struct vnop_pagein_args {
 struct vnodeop_desc *a_desc,
 struct vnode 	*a_vp,
 upl_t		a_pl,
 vm_offset_t	a_pl_offset,
 off_t		a_f_offset,
 size_t		a_size,
 int		a_flags
 vfs_context_t	a_context;
 } *ap;
 */
{
    int         error;
    vnop_pagein_args_class apc( ap );
    bool        callOriginal = true;
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_pagein_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
    //
    // process the request to the covering vnode
    //
    if( vnopArgs.vnode1.coveringVnode ){
        
        VifCoveringFsdResult  opResult = {0x0};
        
        assert( vnopArgs.vnode1.coveredVnode );
        
        vnopArgs.fsd->processPagein( &vnopArgs.vnode1,
                                    apc.ap,
                                    &opResult );
        
        callOriginal = ( 0x1 == opResult.flags.passThrough );
        
        if( !callOriginal ){
            
            assert( 0x1 == opResult.flags.completeWithStatus );
            error = opResult.status;
        }
        
    } // if( vnopArgs.vnode1.coveringVnode )
#endif // _VIF_MACOSX_VFS_ISOLATION
    
    if( callOriginal ){
        
        assert( vnode_fsnode( apc.ap->a_vp ) );
        
        int (*orig)(struct vnop_pagein_args *ap);
        
        orig = (int (*)(struct vnop_pagein_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_pagein );
        assert( orig );
        
        error = orig( apc.ap );
        
    } // end if( callOriginal )
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        /*
         * XXX
         * perhaps the size of the underlying object has changed under
         * our feet.  take advantage of the offset information present
         * in the uio structure.
         */
        if( !error ){
            
            off_t cur = apc.ap->a_f_offset + (off_t)apc.ap->a_pl_offset;
            
            vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode1, cur, false );
            
        }// end if( !error )
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdPageoutHook(struct vnop_pageout_args *ap)
/*
 struct vnop_pageout_args {
 struct vnodeop_desc *a_desc,
 struct vnode 	*a_vp,
 upl_t		a_pl,
 vm_offset_t	a_pl_offset,
 off_t		a_f_offset,
 size_t		a_size,
 int		a_flags
 vfs_context_t	a_context;
 } *ap;
 */
{
    int          error;
    bool         uplProcessed = false;
    bool         callOriginal = true;
    
    vnop_pageout_args_class apc( ap );
    
    //
    // call the original routine
    //
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_pageout_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        callOriginal = false;
    }
    
    //
    // the covering vnode is exported to user
    // apc.ap->a_vp is hooked vnode after processing by getInputArguments !
    //
    assert( vnode_fsnode( apc.ap->a_vp ) );
    //
    // process the request to the covering vnode
    //
    if( callOriginal && vnopArgs.vnode1.dldCoveringVnode && vnopArgs.vnode1.dldCoveringVnode->isControlledByIsolationFilter() ){
        
        VifCoveringFsdResult  opResult = {0x0};
        
        assert( vnopArgs.vnode1.coveredVnode );
        
        vnopArgs.fsd->processPageout( &vnopArgs.vnode1,
                                     apc.ap,
                                     &opResult );
        uplProcessed = true;
        callOriginal = ( 0x1 == opResult.flags.passThrough );
        
        if( !callOriginal ){
            
            assert( 0x1 == opResult.flags.completeWithStatus );
            error = opResult.status;
        }
        
    } // if( callOriginal && vnopArgs.vnode1.coveringVnode )
#endif // _VIF_MACOSX_VFS_ISOLATION
    
    if( callOriginal ){
        
        int (*orig)(struct vnop_pageout_args *ap);
        
        orig = (int (*)(struct vnop_pageout_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_pageout );
        assert( orig );
        
        //
        // call the original, the shadowing will be performed concurrently
        //
        error = orig( apc.ap );
        uplProcessed = true;
        
    }// end if( callOriginal )
    
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        /*
         * XXX
         * perhaps the size of the underlying object has changed under
         * our feet.  take advantage of the offset information present
         * in the uio structure.
         */
        assert( !error );
        if( !error ){
            
            off_t cur = apc.ap->a_f_offset + (off_t)apc.ap->a_pl_offset;
            
            vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode1, cur, false );
            
        }// end if( !error )
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    if( ap->a_pl && !uplProcessed && !(ap->a_flags & UPL_NOCOMMIT) ){
        
        //
        // a caler requires the upl to be freed by the callee
        //
        ubc_upl_abort_range(ap->a_pl, ap->a_pl_offset, ap->a_size, UPL_ABORT_FREE_ON_EMPTY);
    }
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdStrategytHook(struct vnop_strategy_args *ap)
/*
 struct vnop_strategy_args {
 struct vnodeop_desc *a_desc;
 struct buf          *a_bp;
 };
 */
{
    int     error;
    
    vnop_strategy_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_strategy_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( buf_vnode( apc.ap->a_bp ) );
    assert( vnode_fsnode( buf_vnode( apc.ap->a_bp ) ) );
    
    int (*orig)(struct vnop_strategy_args *ap);
    
    orig = (int (*)(struct vnop_strategy_args*))VifGetOriginalVnodeOp( buf_vnode( apc.ap->a_bp ), FltVopEnum_strategy );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdAdvlockHook(struct vnop_advlock_args *ap)
/*
 struct vnop_advlock_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 caddr_t a_id;
 int a_op;
 struct flock *a_fl;
 int a_flags;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_advlock_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_advlock_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_advlock_args *ap);
    
    orig = (int (*)(struct vnop_advlock_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_advlock );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdAllocateHook(struct vnop_allocate_args *ap)
/*
 struct vnop_allocate_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 off_t a_length;
 u_int32_t a_flags;
 off_t *a_bytesallocated;
 off_t a_offset;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_allocate_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_allocate_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_allocate_args *ap);
    
    orig = (int (*)(struct vnop_allocate_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_allocate );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdBlktooffHook(struct vnop_blktooff_args *ap)
/*
 struct vnop_blktooff_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 daddr64_t a_lblkno;
 off_t *a_offset;
 };
 */
{
    int          error;
    vnop_blktooff_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_blktooff_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_blktooff_args *ap);
    
    orig = (int (*)(struct vnop_blktooff_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_blktooff );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}


//--------------------------------------------------------------------

int
VifFsdBlockmapHook(struct vnop_blockmap_args *ap)
/*
 struct vnop_blockmap_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 off_t                a_foffset;
 size_t               a_size;
 daddr64_t           *a_bpn;
 size_t              *a_run;
 void                *a_poff;
 int                  a_flags;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_blockmap_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_blockmap_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_blockmap_args *ap);
    
    orig = (int (*)(struct vnop_blockmap_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_blockmap );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdBwriteHook(struct vnop_bwrite_args *ap)
/*
 struct vnop_bwrite_args {
 struct vnodeop_desc *a_desc;
 buf_t a_bp;
 }
 */
{
    int     error;
    vnop_bwrite_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_bwrite_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
#endif // _VIF_MACOSX_VFS_ISOLATION
    
    assert( buf_vnode( apc.ap->a_bp ) && vnode_fsnode( buf_vnode( apc.ap->a_bp ) ) );
    
    int (*orig)(struct vnop_bwrite_args *ap);
    
    orig = (int (*)(struct vnop_bwrite_args*))VifGetOriginalVnodeOp( buf_vnode( apc.ap->a_bp ), FltVopEnum_bwrite );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdCopyfileHook(struct vnop_copyfile_args *ap)
/*
 struct vnop_copyfile_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_fvp;
 vnode_t a_tdvp;
 vnode_t a_tvp;
 struct componentname *a_tcnp;
 int a_mode;
 int a_flags;
 vfs_context_t a_context;
 }
 */
{
    int          error;
    vnop_copyfile_args_class   apc( ap );
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_copyfile_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_fvp ) );
    assert( vnode_fsnode( apc.ap->a_tvp ) );
    
    int (*orig)(struct vnop_copyfile_args *ap);
    
    orig = (int (*)(struct vnop_copyfile_args*))VifGetOriginalVnodeOp( apc.ap->a_fvp, FltVopEnum_copyfile );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdExchangeHook(struct vnop_exchange_args *ap)
/*
 struct vnop_exchange_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_fvp;
 vnode_t a_tvp;
 int a_options;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_exchange_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_exchange_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_fvp ) );
    assert( vnode_fsnode( apc.ap->a_tvp ) );
    
    int (*orig)(struct vnop_exchange_args *ap);
    
    orig = (int (*)(struct vnop_exchange_args*))VifGetOriginalVnodeOp( apc.ap->a_fvp, FltVopEnum_exchange );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdFsyncHook(struct vnop_fsync_args *ap)
/*
 struct vnop_fsync_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 int a_waitfor;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_fsync_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_fsync_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        vnode_t coveringVnode = vnopArgs.vnode1.coveringVnode;
        assert( vnopArgs.vnode1.coveredVnode );
        
        //
        // flush the cache
        // TO DO this is to avoid a deadlock when the system waits forever
        // on unmount during the shutdown because of dirty pages, the reason
        // must be investigated and fixed dicently
        //
        cluster_push( coveringVnode, IO_SYNC );
        
    } // end if( vnopArgs.vnode1.coveringVnode )
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_fsync_args *ap);
    
    orig = (int (*)(struct vnop_fsync_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_fsync );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdGetattrHook(struct vnop_getattr_args *ap)
/*
 struct vnop_getattr_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 struct vnode_attr *a_vap;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_getattr_args_class    apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_getattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_getattr_args *ap);
    
    orig = (int (*)(struct vnop_getattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_getattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.fsd && vnopArgs.vnode1.coveringVnode &&
       ( KERN_SUCCESS == error ) && VATTR_IS_ACTIVE( apc.ap->a_vap, va_data_size ) )
        vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode1, apc.ap->a_vap->va_data_size, true );
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdGetxattrHook(struct vnop_getxattr_args *ap)
/*
 struct vnop_getxattr_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 const char * a_name;
 uio_t a_uio;
 size_t *a_size;
 int a_options;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_getxattr_args_class    apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_getxattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_getxattr_args *ap);
    
    orig = (int (*)(struct vnop_getxattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_getxattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdIoctlHook(struct vnop_ioctl_args *ap)
/*
 struct vnop_ioctl_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 u_long a_command;
 caddr_t a_data;
 int a_fflag;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_ioctl_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_ioctl_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_ioctl_args *ap);
    
    orig = (int (*)(struct vnop_ioctl_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_ioctl );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdLinkHook(struct vnop_link_args *ap)
/*
 struct vnop_link_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_vp;
 vnode_t               a_tdvp;
 struct componentname *a_cnp;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_link_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_link_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_link_args *ap);
    
    orig = (int (*)(struct vnop_link_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_link );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdListxattrHook(struct vnop_listxattr_args *ap)
/*
 struct vnop_listxattr_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 uio_t                a_uio;
 size_t              *a_size;
 int                  a_options;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_listxattr_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_listxattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_listxattr_args *ap);
    
    orig = (int (*)(struct vnop_listxattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_listxattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdKqfiltAddHook(struct vnop_kqfilt_add_args *ap)
/*
 struct vnop_kqfilt_add_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_vp;
 struct knote         *a_kn;
 struct proc          *p;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_kqfilt_add_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_kqfilt_add_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_kqfilt_add_args *ap);
    
    orig = (int (*)(struct vnop_kqfilt_add_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_kqfilt_add );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdKqfiltRemoveHook(__unused struct vnop_kqfilt_remove_args *ap)
/*
 struct vnop_kqfilt_remove_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_vp;
 uintptr_t             ident;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_kqfilt_remove_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_kqfilt_remove_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_kqfilt_remove_args *ap);
    
    orig = (int (*)(struct vnop_kqfilt_remove_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_kqfilt_remove );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdMkdirHook(struct vnop_mkdir_args *ap)
/*
 struct vnop_mkdir_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_dvp;
 vnode_t              *a_vpp;
 struct componentname *a_cnp;
 struct vnode_attr    *a_vap;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_mkdir_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_mkdir_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_dvp ) );
    
    int (*orig)(struct vnop_mkdir_args *ap);
    
    orig = (int (*)(struct vnop_mkdir_args*))VifGetOriginalVnodeOp( apc.ap->a_dvp, FltVopEnum_mkdir );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdMknodHook(struct vnop_mknod_args *ap)
/*
 struct vnop_mknod_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_dvp;
 struct vnode **a_vpp;
 struct componentname *a_cnp;
 struct vnode_attr *a_vap;
 vfs_context_t a_context;
 } *ap;
 */
{
    int          error;
    vnop_mknod_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_mknod_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_dvp ) );
    
    int (*orig)(struct vnop_mknod_args *ap);
    
    orig = (int (*)(struct vnop_mknod_args*))VifGetOriginalVnodeOp( apc.ap->a_dvp, FltVopEnum_mknod );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}


//--------------------------------------------------------------------

int
VifFsdMmapHook(struct vnop_mmap_args *ap)
/*
 struct vnop_mmap_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 int                  a_fflags;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_mmap_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_mmap_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_mmap_args *ap);
    
    orig = (int (*)(struct vnop_mmap_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_mmap );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdMnomapHook(struct vnop_mnomap_args *ap)
/*
 struct vnop_mnomap_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_mnomap_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_mnomap_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_mnomap_args *ap);
    
    orig = (int (*)(struct vnop_mnomap_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_mnomap );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

/*
 struct vnop_offtoblk_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 off_t                a_offset;
 daddr64_t           *a_lblkno;
 };
 */
int
VifFsdOfftoblkHook(struct vnop_offtoblk_args *ap)
{
    int          error;
    vnop_offtoblk_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_offtoblk_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_offtoblk_args *ap);
    
    orig = (int (*)(struct vnop_offtoblk_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_offtoblock );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdPathconfHook(struct vnop_pathconf_args *ap)
/*
 struct vnop_pathconf_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 int                  a_name;
 int                 *a_retval;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_pathconf_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_pathconf_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_pathconf_args *ap);
    
    orig = (int (*)(struct vnop_pathconf_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_pathconf );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdReaddirHook(struct vnop_readdir_args *ap)
/*
 struct vnop_readdir_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 struct uio          *a_uio;
 int                  a_flags;
 int                 *a_eofflag;
 int                 *a_numdirent;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_readdir_args_class  apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_readdir_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_readdir_args *ap);
    
    orig = (int (*)(struct vnop_readdir_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_readdir );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdReaddirattrHook(struct vnop_readdirattr_args *ap)
/*
 struct vnop_readdirattr_args {
 struct vnode *a_vp;
 struct attrlist *a_alist;
 struct uio *a_uio;
 u_long a_maxcount;
 u_long a_options;
 u_long *a_newstate;
 int *a_eofflag;
 u_long *a_actualcount;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_readdirattr_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_readdirattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_readdirattr_args *ap);
    
    orig = (int (*)(struct vnop_readdirattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_readdirattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdReadlinkHook(struct vnop_readlink_args *ap)
/*
 struct vnop_readlink_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 struct uio          *a_uio;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_readlink_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_readlink_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_readlink_args *ap);
    
    orig = (int (*)(struct vnop_readlink_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_readlink );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdRemoveHook(struct vnop_remove_args *ap)
/*
 struct vnop_remove_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_dvp;
 vnode_t               a_vp;
 struct componentname *a_cnp;
 int                   a_flags;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_remove_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_remove_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_remove_args *ap);
    
    orig = (int (*)(struct vnop_remove_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_remove );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdRemovexattrHook(struct vnop_removexattr_args *ap)
/*
 struct vnop_removexattr_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 char                *a_name;
 int                  a_options;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_removexattr_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_removexattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_removexattr_args *ap);
    
    orig = (int (*)(struct vnop_removexattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_removexattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdRevokeHook(struct vnop_revoke_args *ap)
/*
 *  struct vnop_revoke_args {
 *      struct vnodeop_desc  *a_desc;
 *      vnode_t               a_vp;
 *      int                   a_flags;
 *      vfs_context_t         a_context;
 *  };
 */
{
    int          error;
    vnop_revoke_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_revoke_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_revoke_args *ap);
    
    orig = (int (*)(struct vnop_revoke_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_revoke );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.vnode1.coveringVnode ){
        
        apc.ap->a_vp = vnopArgs.vnode1.coveringVnode;
        
        //
        // revoke the covering vnode
        //
        nop_revoke( apc.ap );
    }
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdRenameHook(struct vnop_rename_args *ap)
/*
 struct vnop_rename_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_fdvp;
 vnode_t               a_fvp;
 struct componentname *a_fcnp;
 vnode_t               a_tdvp;
 vnode_t               a_tvp;
 struct componentname *a_tcnp;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_rename_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_rename_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_fvp ) );
    assert( apc.ap->a_tvp ? ( NULL != vnode_fsnode( apc.ap->a_tvp ) ) : true );
    
    int (*orig)(struct vnop_rename_args *ap);
    
    orig = (int (*)(struct vnop_rename_args*))VifGetOriginalVnodeOp( apc.ap->a_fvp, FltVopEnum_rename );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( KERN_SUCCESS == error ){
        
        if( vnopArgs.vnode2.coveringVnode ){
            
            assert( vnopArgs.vnode2.coveredVnode == ap->a_tvp );
            
            vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode2, 0x0, true );
            
            //
            // copy the sparse file from the sourve vnode to the destanation vnode
            //
            if( vnopArgs.vnode2.dldCoveringVnode->sparseFile != vnopArgs.vnode1.dldCoveringVnode->sparseFile ){
                
                VifSparseFile* sparseFileToCopy = NULL;
                VifIOVnode*    fromVifVnode = vnopArgs.vnode1.dldCoveringVnode;
                VifIOVnode*    toVifVnode   = vnopArgs.vnode2.dldCoveringVnode;
                bool           removeFromHashTable = false;
                
                assert( VifIOVnode::kVnodeType_CoveringFSD == fromVifVnode->dldVnodeType &&
                       VifIOVnode::kVnodeType_CoveringFSD == toVifVnode->dldVnodeType );
                
                //
                // the two stage copy is required as there is no
                // lock hierarchy between VifIOVnode locks,
                // the possible hierarchy might use an object
                // address
                //
                assert( fromVifVnode->spinLock );
                IOSimpleLockLock( fromVifVnode->spinLock );
                { // start of the lock
                    
                    assert( !preemption_enabled() );
                    
                    sparseFileToCopy = fromVifVnode->sparseFile;
                    if( NULL != sparseFileToCopy )
                        sparseFileToCopy->retain();
                    
                } // end of the lock
                IOSimpleLockUnlock( fromVifVnode->spinLock );
                
                if( sparseFileToCopy && !sparseFileToCopy->incrementUsersCount() ){
                    
                    assert(!"sparseFileToCopy->incrementUsersCount() failed for rename");
                    DBG_PRINT_ERROR(("sparseFileToCopy->incrementUsersCount() failed for rename\n"));
                    sparseFileToCopy->release();
                    sparseFileToCopy = NULL;
                }
                
                VifSparseFile* sparseFileToDelete = NULL;
                
                assert( toVifVnode->spinLock );
                IOSimpleLockLock( toVifVnode->spinLock );
                { // start of the lock
                    
                    assert( !preemption_enabled() );
                    
                    sparseFileToDelete = toVifVnode->sparseFile;
                    toVifVnode->sparseFile = sparseFileToCopy;
                    
                    if( sparseFileToCopy ){
                        
                        sparseFileToCopy->exchangeIsolationRelatedVnode( toVifVnode->vnode );
                        removeFromHashTable = ( sparseFileToCopy != sparseFileToDelete );
                        sparseFileToCopy = NULL; // the ownership has been transfered to toVifVnode object
                    }
                    
                } // end of the lock
                IOSimpleLockUnlock( toVifVnode->spinLock );
                
                if( sparseFileToDelete ){
                    
                    //
                    // if this was a last user the entry will be removed from the hash table
                    //
                    sparseFileToDelete->decrementUsersCount();
                    
                    /*
                     //
                     // remove from the hash, removed as done by decrementUsersCount()
                     //
                     if( removeFromHashTable ){
                     
                     assert( VifSparseFilesHashTable::sSparseFilesHashTable );
                     VifSparseFilesHashTable::sSparseFilesHashTable->RemoveEntryByObject( sparseFileToDelete );
                     }
                     */
                    
                    sparseFileToDelete->release();
                    sparseFileToDelete = NULL;
                    
                } // end if( sparseFileToDelete )
                
            } // end if( vnopArgs.vnode2.dldCoveringVnode->sparseFile != vnopArgs.vnode1.dldCoveringVnode->sparseFile ){
            
        } // end if( vnopArgs.vnode2.coveringVnode )
    } // end if( KERN_SUCCESS == error )
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

/*
 struct vnop_rmdir_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_dvp;
 vnode_t               a_vp;
 struct componentname *a_cnp;
 vfs_context_t         a_context;
 };
 */
int
VifFsdRmdirHook(struct vnop_rmdir_args *ap)
{
    int          error;
    vnop_rmdir_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_rmdir_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    assert( vnode_fsnode( apc.ap->a_dvp ) );
    
    int (*orig)(struct vnop_rmdir_args *ap);
    
    orig = (int (*)(struct vnop_rmdir_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_rmdir );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdSearchfsHook(struct vnop_searchfs_args *ap)
/*
 struct vnop_searchfs_args{
 struct vnodeop_desc *a_desc;
 struct vnode *a_vp;
 void *a_searchparams1;
 void *a_searchparams2;
 struct attrlist *a_searchattrs;
 u_long a_maxmatches;
 struct timeval *a_timelimit;
 struct attrlist *a_returnattrs;
 u_long *a_nummatches;
 u_long a_scriptcode;
 u_long a_options;
 struct uio *a_uio;
 struct searchstate *a_searchstate;
 vfs_context_t a_context;
 }
 */
{
    int          error;
    vnop_searchfs_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_searchfs_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_searchfs_args *ap);
    
    orig = (int (*)(struct vnop_searchfs_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_searchfs );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

/*
 struct vnop_select_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 int                  a_which;
 int                  a_fflags;
 void                *a_wql;
 vfs_context_t        a_context;
 };
 */
int
VifFsdSelectHook(__unused struct vnop_select_args *ap)
{
    int          error;
    vnop_select_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_select_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_select_args *ap);
    
    orig = (int (*)(struct vnop_select_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_select );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

/*
 struct vnop_setattr_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 struct vnode_attr   *a_vap;
 vfs_context_t        a_context;
 };
 */
int
VifFsdSetattrHook(struct vnop_setattr_args *ap)
{
    int          error;
    vnop_setattr_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_setattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_setattr_args *ap);
    
    orig = (int (*)(struct vnop_setattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_setattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    if( vnopArgs.fsd && vnopArgs.vnode1.coveringVnode &&
       ( KERN_SUCCESS == error ) && VATTR_IS_ACTIVE(ap->a_vap, va_data_size) )
        vnopArgs.fsd->VifSetUbcFileSize( &vnopArgs.vnode1, apc.ap->a_vap->va_data_size, true );
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdSetxattrHook(struct vnop_setxattr_args *ap)
/*
 struct vnop_setxattr_args {
 struct vnodeop_desc *a_desc;
 vnode_t              a_vp;
 char                *a_name;
 uio_t                a_uio;
 int                  a_options;
 vfs_context_t        a_context;
 };
 */
{
    int          error;
    vnop_setxattr_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_setxattr_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_setxattr_args *ap);
    
    orig = (int (*)(struct vnop_setxattr_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_setxattr );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

int
VifFsdSymlinkHook(struct vnop_symlink_args *ap)
/*
 struct vnop_symlink_args {
 struct vnodeop_desc  *a_desc;
 vnode_t               a_dvp;
 vnode_t              *a_vpp;
 struct componentname *a_cnp;
 struct vnode_attr    *a_vap;
 char                 *a_target;
 vfs_context_t         a_context;
 };
 */
{
    int          error;
    vnop_symlink_args_class   apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_symlink_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( ap->a_dvp ) );
    
    int (*orig)(struct vnop_symlink_args *ap);
    
    orig = (int (*)(struct vnop_symlink_args*))VifGetOriginalVnodeOp( ap->a_dvp, FltVopEnum_symlink );
    assert( orig );
    
    error = orig( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdWhiteoutHook(struct vnop_whiteout_args *ap)
/*
 struct vnop_whiteout_args {
 struct vnodeop_desc *a_desc;
 struct vnode *a_dvp;
 struct componentname *a_cnp;
 int a_flags;
 vfs_context_t a_context;
 } *ap;
 */
{
    int          error;
    vnop_whiteout_args_class apc( ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_whiteout_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_dvp ) );
    
    int (*orig)(struct vnop_whiteout_args *ap);
    
    orig = (int (*)(struct vnop_whiteout_args*))VifGetOriginalVnodeOp( apc.ap->a_dvp, FltVopEnum_whiteout );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

#ifdef __APPLE_API_UNSTABLE
#if NAMEDSTREAMS

//--------------------------------------------------------------------

static int
VifFsdGetnamedstreamHook(struct vnop_getnamedstream_args *ap)
/*
 struct vnop_getnamedstream_args {
	struct vnodeop_desc *a_desc;
	vnode_t a_vp;
	vnode_t *a_svpp;
	const char *a_name;
	enum nsoperation a_operation;
	int a_flags;
	vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_getnamedstream_args_class   apc( ap );
    
    //
    // TO DO - process a_svpp
    //
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_getnamedstream_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_getnamedstream_args *ap);
    
    orig = (int (*)(struct vnop_getnamedstream_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_getnamedstreamHook );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdMakenamedstreamHook(struct vnop_makenamedstream_args *ap)
/*
 struct vnop_makenamedstream_args {
 struct vnodeop_desc *a_desc;
 vnode_t *a_svpp;
 vnode_t a_vp;
 const char *a_name;
 int a_flags;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_makenamedstream_args_class   apc( ap );
    
    //
    // TO DO - process a_svpp
    //
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_makenamedstream_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_makenamedstream_args *ap);
    
    orig = (int (*)(struct vnop_makenamedstream_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_makenamedstreamHook );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

static int
VifFsdRemovenamedstreamHook(struct vnop_removenamedstream_args *ap)
/*
 struct vnop_removenamedstream_args {
 struct vnodeop_desc *a_desc;
 vnode_t a_vp;
 vnode_t a_svp;
 const char *a_name;
 int a_flags;
 vfs_context_t a_context;
 };
 */
{
    int          error;
    vnop_removenamedstream_args_class  apc( ap );
    
    //
    // TO DO - process a_svpp
    //
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    VifVnopArgs<vnop_removenamedstream_args_class>   vnopArgs;
    
    //
    // after returning the apc.ap->a_vp can be replaced on covered vnode!
    // even apc.ap might be changed, so always use apc.ap instead of ap
    //
    error = vnopArgs.getInputArguments( &apc );
    assert( !error );
    if( KERN_SUCCESS != error ){
        
        DBG_PRINT_ERROR(("vnodArgs.getInputArguments( &apc ) failed"));
        return error;
    }
    
    assert( false == vnopArgs.fsd->VifIsCoveringVnode( apc.ap->a_svp ) );
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    assert( vnode_fsnode( apc.ap->a_vp ) );
    
    int (*orig)(struct vnop_removenamedstream_args *ap);
    
    orig = (int (*)(struct vnop_removenamedstream_args*))VifGetOriginalVnodeOp( apc.ap->a_vp, FltVopEnum_removenamedstreamHook );
    assert( orig );
    
    error = orig( apc.ap );
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    vnopArgs.putInputArguments( &apc );
    
#endif//_VIF_MACOSX_VFS_ISOLATION
    
    return error;
}

//--------------------------------------------------------------------

#endif // __APPLE_API_UNSTABLE
#endif // NAMEDSTREAMS

//--------------------------------------------------------------------

//
// the following two arrays must obey the rule - for any entry in the gVifVnodeVopHookEntries array
// there must be a corresponding entry in the gVifVnodeVopEnumEntries array and in the VnodeOpvOffsetDesc
// gVifFakeVnodeopEntries arrays, vice versa is not true - the last three mentioned arrays might contain
// more descriptors than the gVifVnodeVopHookEntries array, so they can be filled in advance
//

//
//
// hook descriptors for the vnode's o_pv vector,
// this array is iterated when hooking is performed,
// so it defines the order of hooks, the correct
// order is when reclaim and close are hooked first
// and the open and create are last so there will not be
// a case when a vnode is discovered on create but its
// reclaim is missed as the reclaim routine has not yet been
// hooked
//


//
// the lookup and create must be hooked ater all over vnode operations
// to provide correct processing for covered vnodes when it will not happen
// that covering vnode will sneak to the covered FSD
//

#ifndef _VIF_MACOSX_VFS_ISOLATION

struct vnodeopv_entry_desc gVifVnodeVopHookEntries[] = {
    { &vnop_reclaim_desc,  (VOPFUNC)VifFsdReclaimHook },            /* reclaim */
    { &vnop_close_desc,    (VOPFUNC)VifFsdCloseHook },		        /* close */
    { &vnop_access_desc,   (VOPFUNC)VifFsdAccessHook },             /* access */
    { &vnop_inactive_desc, (VOPFUNC)VifFsdInactiveHook },	        /* inactive */
    { &vnop_read_desc,     (VOPFUNC)VifFsdReadHook },		        /* read */
    { &vnop_write_desc,    (VOPFUNC)VifFsdWriteHook },		        /* write */
    { &vnop_pagein_desc,   (VOPFUNC)VifFsdPageinHook },		        /* Pagein */
    { &vnop_pageout_desc,  (VOPFUNC)VifFsdPageoutHook },		    /* Pageout */
    { &vnop_strategy_desc, (VOPFUNC)VifFsdStrategytHook },          /* Strategy */
    { &vnop_open_desc,     (VOPFUNC)VifFsdOpenHook },		        /* open */
    { &vnop_lookup_desc,   (VOPFUNC)VifFsdLookupHook },		        /* lookup */
    { &vnop_create_desc,   (VOPFUNC)VifFsdCreateHook },		        /* create */
    { &vnop_mmap_desc,     (VOPFUNC)VifFsdMmapHook  },              /* mmap */
    { &vnop_rename_desc,   (VOPFUNC)VifFsdRenameHook },
    { &vnop_pathconf_desc, (VOPFUNC)VifFsdPathconfHook},
    { (struct vnodeop_desc*)NULL, (VOPFUNC)(int(*)())NULL }
};

//
// defines a mapping from the lookup entries to indices
//
struct vnodeopv_entry_desc gVifVnodeVopEnumEntries[] = {
    { &vnop_lookup_desc,   (VOPFUNC)FltVopEnum_lookup },		    /* lookup */
    { &vnop_create_desc,   (VOPFUNC)FltVopEnum_create },		    /* create */
    { &vnop_open_desc,     (VOPFUNC)FltVopEnum_open },		        /* open */
    { &vnop_close_desc,    (VOPFUNC)FltVopEnum_close },		        /* close */
    { &vnop_access_desc,   (VOPFUNC)FltVopEnum_access },            /* access */
    { &vnop_inactive_desc, (VOPFUNC)FltVopEnum_inactive },	        /* inactive */
    { &vnop_reclaim_desc,  (VOPFUNC)FltVopEnum_reclaim },           /* reclaim */
    { &vnop_read_desc,     (VOPFUNC)FltVopEnum_read },		        /* read */
    { &vnop_write_desc,    (VOPFUNC)FltVopEnum_write },		        /* write */
    { &vnop_pagein_desc,   (VOPFUNC)FltVopEnum_pagein },		    /* Pagein */
    { &vnop_pageout_desc,  (VOPFUNC)FltVopEnum_pageout },		    /* Pageout */
    { &vnop_strategy_desc, (VOPFUNC)FltVopEnum_strategy },          /* Strategy */
    { &vnop_mmap_desc,     (VOPFUNC)FltVopEnum_mmap  },             /* mmap */
    { &vnop_rename_desc,   (VOPFUNC)FltVopEnum_rename },
    { &vnop_pathconf_desc, (VOPFUNC)FltVopEnum_pathconf },
    { (struct vnodeop_desc*)NULL, (VOPFUNC)FltVopEnum_Max }
};

#else // _VIF_MACOSX_VFS_ISOLATION

struct vnodeopv_entry_desc gVifVnodeVopHookEntries[] = {
    { &vnop_access_desc,        (VOPFUNC)VifFsdAccessHook                   },
    { &vnop_advlock_desc,       (VOPFUNC)VifFsdAdvlockHook                  },
    { &vnop_allocate_desc,      (VOPFUNC)VifFsdAllocateHook                 },
    { &vnop_blktooff_desc,      (VOPFUNC)VifFsdBlktooffHook                 },
    { &vnop_blockmap_desc,      (VOPFUNC)VifFsdBlockmapHook                 },
    { &vnop_bwrite_desc,        (VOPFUNC)VifFsdBwriteHook                   },
    { &vnop_close_desc,         (VOPFUNC)VifFsdCloseHook                    },
    { &vnop_copyfile_desc,      (VOPFUNC)VifFsdCopyfileHook                 },
    { &vnop_exchange_desc,      (VOPFUNC)VifFsdExchangeHook                 },
    { &vnop_fsync_desc,         (VOPFUNC)VifFsdFsyncHook                    },
    { &vnop_getattr_desc,       (VOPFUNC)VifFsdGetattrHook                  },
    { &vnop_getxattr_desc,      (VOPFUNC)VifFsdGetxattrHook                 },
    { &vnop_inactive_desc,      (VOPFUNC)VifFsdInactiveHook                 },
    { &vnop_ioctl_desc,         (VOPFUNC)VifFsdIoctlHook                    },
    { &vnop_link_desc,          (VOPFUNC)VifFsdLinkHook                     },
    { &vnop_listxattr_desc,     (VOPFUNC)VifFsdListxattrHook                },
    { &vnop_kqfilt_add_desc,    (VOPFUNC)VifFsdKqfiltAddHook                },
    { &vnop_kqfilt_remove_desc, (VOPFUNC)VifFsdKqfiltRemoveHook             },
    { &vnop_mkdir_desc,         (VOPFUNC)VifFsdMkdirHook                    },
    { &vnop_mknod_desc,         (VOPFUNC)VifFsdMknodHook                    },
    { &vnop_mmap_desc,          (VOPFUNC)VifFsdMmapHook                     },
    { &vnop_mnomap_desc,        (VOPFUNC)VifFsdMnomapHook                   },
    { &vnop_offtoblk_desc,      (VOPFUNC)VifFsdOfftoblkHook                 },
    { &vnop_open_desc,          (VOPFUNC)VifFsdOpenHook                     },
    { &vnop_pagein_desc,        (VOPFUNC)VifFsdPageinHook                   },
    { &vnop_pageout_desc,       (VOPFUNC)VifFsdPageoutHook                  },
    { &vnop_pathconf_desc,      (VOPFUNC)VifFsdPathconfHook                 },
    { &vnop_read_desc,          (VOPFUNC)VifFsdReadHook                     },
    { &vnop_readdir_desc,       (VOPFUNC)VifFsdReaddirHook                  },
    { &vnop_readdirattr_desc,   (VOPFUNC)VifFsdReaddirattrHook              },
    { &vnop_readlink_desc,      (VOPFUNC)VifFsdReadlinkHook                 },
    { &vnop_reclaim_desc,       (VOPFUNC)VifFsdReclaimHook                  },
    { &vnop_remove_desc,        (VOPFUNC)VifFsdRemoveHook                   },
    { &vnop_removexattr_desc,   (VOPFUNC)VifFsdRemovexattrHook              },
    { &vnop_rename_desc,        (VOPFUNC)VifFsdRenameHook                   },
    { &vnop_revoke_desc,        (VOPFUNC)VifFsdRevokeHook                   },
    { &vnop_rmdir_desc,         (VOPFUNC)VifFsdRmdirHook                    },
    { &vnop_searchfs_desc,      (VOPFUNC)VifFsdSearchfsHook                 },
    { &vnop_select_desc,        (VOPFUNC)VifFsdSelectHook                   },
    { &vnop_setattr_desc,       (VOPFUNC)VifFsdSetattrHook                  },
    { &vnop_setxattr_desc,      (VOPFUNC)VifFsdSetxattrHook                 },
    { &vnop_strategy_desc,      (VOPFUNC)VifFsdStrategytHook                },
    { &vnop_symlink_desc,       (VOPFUNC)VifFsdSymlinkHook                  },
    { &vnop_whiteout_desc,      (VOPFUNC)VifFsdWhiteoutHook                 },
    { &vnop_write_desc,         (VOPFUNC)VifFsdWriteHook                    },
    
#ifdef __APPLE_API_UNSTABLE
#if NAMEDSTREAMS
    // TO DO - the following three definitions are from Apple unstable kernel
    // portion and requires name streams to be compiled in the kernel,
    // might prevent the driver from loading
    { &vnop_getnamedstream_desc,    (VOPFUNC)VifFsdGetnamedstreamHook       },
    { &vnop_makenamedstream_desc,   (VOPFUNC)VifFsdMakenamedstreamHook      },
    { &vnop_removenamedstream_desc, (VOPFUNC)VifFsdRemovenamedstreamHook    },
#endif // __APPLE_API_UNSTABLE
#endif // NAMEDSTREAMS
    
    { &vnop_create_desc,        (VOPFUNC)VifFsdCreateHook                   },
    { &vnop_lookup_desc,        (VOPFUNC)VifFsdLookupHook                   },
    
    { (struct vnodeop_desc*)NULL, (VOPFUNC)(int(*)())NULL }
};

//
// defines a mapping from the lookup entries to indices
//
struct vnodeopv_entry_desc gVifVnodeVopEnumEntries[] = {
    { &vnop_access_desc,        (VOPFUNC)FltVopEnum_access                  },
    { &vnop_advlock_desc,       (VOPFUNC)FltVopEnum_advlock                 },
    { &vnop_allocate_desc,      (VOPFUNC)FltVopEnum_allocate                },
    { &vnop_blktooff_desc,      (VOPFUNC)FltVopEnum_blktooff                },
    { &vnop_blockmap_desc,      (VOPFUNC)FltVopEnum_blockmap                },
    { &vnop_bwrite_desc,        (VOPFUNC)FltVopEnum_bwrite                  },
    { &vnop_close_desc,         (VOPFUNC)FltVopEnum_close                   },
    { &vnop_copyfile_desc,      (VOPFUNC)FltVopEnum_copyfile                },
    { &vnop_create_desc,        (VOPFUNC)FltVopEnum_create                  },
    { &vnop_default_desc,       (VOPFUNC)FltVopEnum_default                 },
    { &vnop_exchange_desc,      (VOPFUNC)FltVopEnum_exchange                },
    { &vnop_fsync_desc,         (VOPFUNC)FltVopEnum_fsync                   },
    { &vnop_getattr_desc,       (VOPFUNC)FltVopEnum_getattr                 },
    { &vnop_getxattr_desc,      (VOPFUNC)FltVopEnum_getxattr                },
    { &vnop_inactive_desc,      (VOPFUNC)FltVopEnum_inactive                },
    { &vnop_ioctl_desc,         (VOPFUNC)FltVopEnum_ioctl                   },
    { &vnop_link_desc,          (VOPFUNC)FltVopEnum_link                    },
    { &vnop_listxattr_desc,     (VOPFUNC)FltVopEnum_listxattr               },
    { &vnop_lookup_desc,        (VOPFUNC)FltVopEnum_lookup                  },
    { &vnop_kqfilt_add_desc,    (VOPFUNC)FltVopEnum_kqfilt_add              },
    { &vnop_kqfilt_remove_desc, (VOPFUNC)FltVopEnum_kqfilt_remove           },
    { &vnop_mkdir_desc,         (VOPFUNC)FltVopEnum_mkdir                   },
    { &vnop_mknod_desc,         (VOPFUNC)FltVopEnum_mknod                   },
    { &vnop_mmap_desc,          (VOPFUNC)FltVopEnum_mmap                    },
    { &vnop_mnomap_desc,        (VOPFUNC)FltVopEnum_mnomap                  },
    { &vnop_offtoblk_desc,      (VOPFUNC)FltVopEnum_offtoblock              },
    { &vnop_open_desc,          (VOPFUNC)FltVopEnum_open                    },
    { &vnop_pagein_desc,        (VOPFUNC)FltVopEnum_pagein                  },
    { &vnop_pageout_desc,       (VOPFUNC)FltVopEnum_pageout                 },
    { &vnop_pathconf_desc,      (VOPFUNC)FltVopEnum_pathconf                },
    { &vnop_read_desc,          (VOPFUNC)FltVopEnum_read                    },
    { &vnop_readdir_desc,       (VOPFUNC)FltVopEnum_readdir                 },
    { &vnop_readdirattr_desc,   (VOPFUNC)FltVopEnum_readdirattr             },
    { &vnop_readlink_desc,      (VOPFUNC)FltVopEnum_readlink                },
    { &vnop_reclaim_desc,       (VOPFUNC)FltVopEnum_reclaim                 },
    { &vnop_remove_desc,        (VOPFUNC)FltVopEnum_remove                  },
    { &vnop_removexattr_desc,   (VOPFUNC)FltVopEnum_removexattr             },
    { &vnop_rename_desc,        (VOPFUNC)FltVopEnum_rename                  },
    { &vnop_revoke_desc,        (VOPFUNC)FltVopEnum_revoke                  },
    { &vnop_rmdir_desc,         (VOPFUNC)FltVopEnum_rmdir                   },
    { &vnop_searchfs_desc,      (VOPFUNC)FltVopEnum_searchfs                },
    { &vnop_select_desc,        (VOPFUNC)FltVopEnum_select                  },
    { &vnop_setattr_desc,       (VOPFUNC)FltVopEnum_setattr                 },
    { &vnop_setxattr_desc,      (VOPFUNC)FltVopEnum_setxattr                },
    { &vnop_strategy_desc,      (VOPFUNC)FltVopEnum_strategy                },
    { &vnop_symlink_desc,       (VOPFUNC)FltVopEnum_symlink                 },
    { &vnop_whiteout_desc,      (VOPFUNC)FltVopEnum_whiteout                },
    { &vnop_write_desc,         (VOPFUNC)FltVopEnum_write                   },
    
#ifdef __APPLE_API_UNSTABLE
#if NAMEDSTREAMS
    { &vnop_getnamedstream_desc,   (VOPFUNC)FltVopEnum_getnamedstreamHook   },
    { &vnop_makenamedstream_desc,  (VOPFUNC)FltVopEnum_makenamedstreamHook  },
    { &vnop_removenamedstream_desc,(VOPFUNC)FltVopEnum_removenamedstreamHook},
#endif // __APPLE_API_UNSTABLE
#endif // NAMEDSTREAMS
    
    { (struct vnodeop_desc*)NULL, (VOPFUNC)FltVopEnum_Max }
};

#endif// _VIF_MACOSX_VFS_ISOLATION

//--------------------------------------------------------------------

FltVnodeHooksHashTable* FltVnodeHooksHashTable::sVnodeHooksHashTable = NULL;

//--------------------------------------------------------------------

struct vnodeopv_entry_desc*
VifRetriveVnodeOpvEntryDescByVnodeOpDesc(
                                         __in vnodeopv_entry_desc*  vnodeOpvEntryDescArray,
                                         __in struct vnodeop_desc *opve_op
                                         )
{
    for( int i = 0x0; NULL != vnodeOpvEntryDescArray[ i ].opve_op; ++i ){
        
        if( opve_op == vnodeOpvEntryDescArray[ i ].opve_op )
            return &vnodeOpvEntryDescArray[ i ];
        
    }// end for
    
    return ( vnodeopv_entry_desc* )NULL;
}

//--------------------------------------------------------------------

struct vnodeopv_entry_desc*
VifRetriveVnodeOpvEntryDescByVnodeOp(
                                     __in vnodeopv_entry_desc*  vnodeOpvEntryDescArray,
                                     __in VOPFUNC vnodeOp
                                     )
{
    for( int i = 0x0; NULL != vnodeOpvEntryDescArray[ i ].opve_op; ++i ){
        
        if( vnodeOp == vnodeOpvEntryDescArray[ i ].opve_impl )
            return &vnodeOpvEntryDescArray[ i ];
        
    }// end for
    
    return ( vnodeopv_entry_desc* )NULL;
}

//--------------------------------------------------------------------

VOPFUNC
VifGetOriginalVnodeOp(
                      __in vnode_t      vnode,
                      __in FltVopEnum   indx
                      )
{
    VOPFUNC             original;
    VOPFUNC*            v_op;
    FltVnodeHookEntry*  existingEntry;
    
    assert( preemption_enabled() );
    assert( FltVnodeHooksHashTable::sVnodeHooksHashTable );
    assert( FltVopEnum_Unknown< indx && indx < FltVopEnum_Max );
    
    v_op = FltGetVnodeOpVector( vnode );
    
    FltVnodeHooksHashTable::sVnodeHooksHashTable->LockShared();
    {// start of the lock
        existingEntry = FltVnodeHooksHashTable::sVnodeHooksHashTable->RetrieveEntry( v_op, true );
    }// end of the lock
    FltVnodeHooksHashTable::sVnodeHooksHashTable->UnLockShared();
    
    if( !existingEntry ){
        
        //
        // this is a case of an operations table unhooked in the midle of the hooking
        // function execution, so retrieve the current table entry, this case is
        // a very rare one ( I doubt that it will ever happen ) so there is no need
        // for any optimizahion here
        //
        
        //
        // perform the access under the lock protection to avoid a race condition with a possible ongoing hooking
        //
        FltVnodeHooksHashTable::sVnodeHooksHashTable->LockShared();
        {// start of the lock
            
            //
            // check that the vnode has not been rehooked since the last check
            //
            existingEntry = FltVnodeHooksHashTable::sVnodeHooksHashTable->RetrieveEntry( v_op, true );
            if( !existingEntry ){
                
                VOPFUNC*                 v_op;
                FltVnodeOpvOffsetDesc*   offsetDescEntry;
                vnodeopv_entry_desc*     enumDescEntry;
                
                //
                // get the v_op vector's address
                //
                v_op = FltGetVnodeOpVector( vnode );
                assert( v_op );
                
                //
                // get the enum descriptor for the index
                //
                enumDescEntry = VifRetriveVnodeOpvEntryDescByVnodeOp( gVifVnodeVopEnumEntries, (VOPFUNC)indx );
                assert( enumDescEntry );
                
                //
                // get the offset descriptor using the operation descriptor from the retrieved enum descriptor
                //
                offsetDescEntry = FltRetriveVnodeOpvOffsetDescByVnodeOpDesc( enumDescEntry->opve_op );
                assert( offsetDescEntry );
                assert( VIF_VOP_UNKNOWN_OFFSET != offsetDescEntry->offset );
                assert( enumDescEntry->opve_op == offsetDescEntry->opve_op );
                
                original =  *(VOPFUNC*)((vm_offset_t)v_op + offsetDescEntry->offset);
                
            }// end if( !existingEntry )
            
        }// end of the lock
        FltVnodeHooksHashTable::sVnodeHooksHashTable->UnLockShared();
        
        if( !existingEntry ){
            
            assert( original );
            return original;
            
        }// end if( existingEntry )
        
    }
    
    assert( existingEntry );
    
    original = existingEntry->getOrignalVop( indx );
    
    existingEntry->release();
    
    assert( original );
    if( !original ){
        
        DBG_PRINT_ERROR( ("VifGetOriginalVnodeOp(%p,%d) failed\n", (void*)vnode, (int)indx ) );
    }
    
    return original;
}

//--------------------------------------------------------------------

IOReturn
FltHookVnodeVop(
                __inout vnode_t vnode,
                __inout bool* isVopHooked // if an error is returned the value is not defined
)
{
    VOPFUNC*  v_op;
    IOReturn  RC = kIOReturnSuccess;
    
    assert( preemption_enabled() );
    assert( NULL != FltVnodeHooksHashTable::sVnodeHooksHashTable );
    assert( (VOPFUNC*)VIF_VOP_UNKNOWN_OFFSET != FltGetVnodeOpVector( vnode ) );
    
    if( (VOPFUNC*)VIF_VOP_UNKNOWN_OFFSET == FltGetVnodeOpVector( vnode ) || !FltVnodeHooksHashTable::sVnodeHooksHashTable )
        return kIOReturnError;
    
    if( vnode_isrecycled( vnode ) ){
        
        //
        // the vnode has been put in a dead state
        // by vclean()
        // 	vp->v_mount = dead_mountp;
        //  vp->v_op = dead_vnodeop_p;
        //  vp->v_tag = VT_NON;
        //  vp->v_data = NULL;
        //
        // we should not hook dead_vnodeop_p as hooking
        // it results in processing vnode as a normal one
        // which is not true
        //
        return kIOReturnNoDevice;
        
    } // if( vnode_isrecycled( vnode ) )
    
    //
    // if you are changing the following condition
    // do not forget to change the one in VifUnHookVnodeVop()
    //
    if( VREG != vnode_vtype( vnode ) &&
       VDIR != vnode_vtype( vnode ) &&
       VBLK != vnode_vtype( vnode ) &&
       VCHR != vnode_vtype( vnode ) ){
        
        //
        // we are interested only in disk related vnodes
        //
        *isVopHooked = false;
        return kIOReturnSuccess;
    }
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    //
    // hook the mount's VFS operations, the unhook is done by the unmount hook
    //
    RC = VifVfsMntHook::HookVfsMnt( vnode_mount( vnode ) );
    assert( kIOReturnSuccess == RC );
    if( kIOReturnSuccess != RC ){
        
        DBG_PRINT_ERROR(( "VifVfsMntHook::HookVfsMnt() failed\n" ));
        return RC;
    }
#endif // _VIF_MACOSX_VFS_ISOLATION
    
    //
    // get the v_op vector's address
    //
    v_op = FltGetVnodeOpVector( vnode );
    
    assert( NULL != v_op );
    assert( FltVirtToPhys( (vm_offset_t)v_op ) );
    
    FltVnodeHookEntry*  existingEntry = NULL;
    FltVnodeHookEntry*  newEntry = NULL;
    
    //
    // check that the v_op has not been hooked already, the returned entry is not referenced
    //
    FltVnodeHooksHashTable::sVnodeHooksHashTable->LockShared();
    {// start of the lock
        
        //
        // get a referenced entry
        //
        existingEntry = FltVnodeHooksHashTable::sVnodeHooksHashTable->RetrieveEntry( v_op, true );
        if( existingEntry ){
            
            //
            // account for the new vnode, the increment is an atomic
            // operation, and the hash is protected by the shared lock, so
            // unhook can't sneak in and unhook the vop table as before
            // unhooking reacquires the lock exclusive
            //
            existingEntry->incrementVnodeCounter();
        }
        
    }// end of the lock
    FltVnodeHooksHashTable::sVnodeHooksHashTable->UnLockShared();
    
    if( existingEntry ){
        
        //
        // already hooked
        //
        assert( 0x0 != existingEntry->getVnodeCounter() );
        
        //
        // RetrieveEntry() returns a referenced entry
        //
        existingEntry->release();
        *isVopHooked = true;
        
        return kIOReturnSuccess;
    }
    
    newEntry = FltVnodeHookEntry::newEntry();
    assert( newEntry );
    if( !newEntry )
        return kIOReturnNoMemory;
    
    assert( kIOReturnSuccess == RC );
    FltVnodeHooksHashTable::sVnodeHooksHashTable->LockExclusive();
    {// start of the lock
        
        //
        // check again as the lock has been reacquired
        //
        existingEntry = FltVnodeHooksHashTable::sVnodeHooksHashTable->RetrieveEntry( v_op, false );
        if( existingEntry ){
            
            //
            // account for the new vnode
            //
            existingEntry->incrementVnodeCounter();
            
            goto __exit_with_lock;
        }
        
        //
        // add a new entry before hook, as it is hard to unhook if the add fails
        // the hooks might be already in work waiting on the lock to get the
        // original function(!), as the lock is hold exclusively we are protected
        // from the entry access
        //
        if( !FltVnodeHooksHashTable::sVnodeHooksHashTable->AddEntry( v_op, newEntry ) ){
            
            assert( !"FltVnodeHooksHashTable::sVnodeHooksHashTable->AddEntry failed" );
            DBG_PRINT_ERROR( ( "FltHookVnodeVop()->FltVnodeHooksHashTable::sVnodeHooksHashTable->AddEntry failed\n" ) );
            
            RC = kIOReturnNoMemory;
            goto __exit_with_lock;
            
        }// end if
        
#if defined( DBG )
        OSIncrementAtomic( &gVifVNodeHookCount );
#endif//DBG
        
        //
        // account for the new vnode
        //
        newEntry->incrementVnodeCounter();
        
        //
        // iterate through the registered hooks
        //
        for( int i = 0x0; NULL != gVifVnodeVopHookEntries[ i ].opve_op; ++i ){
            
            FltVnodeOpvOffsetDesc*   offsetDescEntry;
            vnodeopv_entry_desc*     enumDescEntry;
            
            offsetDescEntry = FltRetriveVnodeOpvOffsetDescByVnodeOpDesc( gVifVnodeVopHookEntries[ i ].opve_op );
            assert( offsetDescEntry );
            if( !offsetDescEntry ){
                
                DBG_PRINT_ERROR( ( "FltHookVnodeVop()->FltRetriveVnodeOpvOffsetDescByVnodeOpDesc(%d) failed\n", i ) );
                continue;
            }
            
            enumDescEntry = VifRetriveVnodeOpvEntryDescByVnodeOpDesc( gVifVnodeVopEnumEntries, offsetDescEntry->opve_op );
            assert( enumDescEntry );
            if( !enumDescEntry ){
                
                DBG_PRINT_ERROR( ( "FltHookVnodeVop()->VifRetriveVnodeOpvEntryDescByVnodeOpDesc(%d) failed\n", i ) );
                continue;
            }
            
            assert( offsetDescEntry->opve_op == enumDescEntry->opve_op );
            assert( VIF_VOP_UNKNOWN_OFFSET != offsetDescEntry->offset );
            assert( gVifVnodeVopHookEntries[ i ].opve_impl );
            
            VOPFUNC       original;
            VOPFUNC       hook;
            FltVopEnum    indx;
            unsigned int  bytes;
            
            original = *(VOPFUNC*)((vm_offset_t)v_op + offsetDescEntry->offset);
            hook = gVifVnodeVopHookEntries[ i ].opve_impl;
            indx = *(FltVopEnum*)(&enumDescEntry->opve_impl);// just to calm the compiler
            
            assert( hook && original );
            
            if( ( VREG != vnode_vtype( vnode ) && VDIR != vnode_vtype( vnode ) ) &&
               &vnop_strategy_desc == gVifVnodeVopHookEntries[ i ].opve_op ){
                
                //
                // do not hook the device's vnode strategic routine as
                // it is hard to define the device's vnode from the strategic
                // routine's parameters, the only present vnode is for
                // the real FSD's vnode which initiated IO, using it is
                // impossible as this gives an infinite recursion - for
                // example in the HFS case the sequence of calls is
                /*
                 #169 0x0031db1a in buf_strategy (devvp=0x57b4f38, ap=0x31d237f4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_bio.c:951
                 #170 0x004c3e26 in hfs_vnop_strategy (ap=0x31d237f4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/hfs/hfs_readwrite.c:2539
                 #171 0x4642d0a3 in VifFsdStrategytHook (ap=0x31d237f4) at /work/DeviceLockProject/DeviceLockIOKitDriver/VifVNodeHook.cpp:583
                 #172 0x00359e34 in VNOP_STRATEGY (bp=0x2d833500) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/kpi_vfs.c:5885
                 #173 0x00322d3b in cluster_io (vp=0x7d85cb8, upl=0x604a480, upl_offset=4096, f_offset=4096, non_rounded_size=0, flags=5, real_bp=0x0, iostate=0x31d23a08, callback=0, callback_arg=0 0) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_cluster.c:1410
                 #174 0x00327736 in cluster_read_copy (vp=0x7d85cb8, uio=0x31d23e60, io_req_size=512, filesize=2097152, flags=2048, callback=0, callback_arg=0x0) at /work/Mac_OS_X_kernel/10_6_4/xnu 1504.7.4/bsd/vfs/vfs_cluster.c:3565
                 #175 0x0032896c in cluster_read_direct (vp=0x7d85cb8, uio=0x31d23e60, filesize=2097152, read_type=0x31d23c40, read_length=0x31d23c44, flags=2048, callback=0, callback_arg=0x0) at / ork/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_cluster.c:4197
                 #176 0x00326c21 in cluster_read_ext (vp=0x7d85cb8, uio=0x31d23e60, filesize=2097152, xflags=2048, callback=0, callback_arg=0x0) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs vfs_cluster.c:3257
                 #177 0x00326a7b in cluster_read (vp=0x7d85cb8, uio=0x31d23e60, filesize=2097152, xflags=2048) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_cluster.c:3207
                 #178 0x004c52d3 in hfs_vnop_read (ap=0x31d23d80) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/hfs/hfs_readwrite.c:174
                 #179 0x4642dae1 in VifFsdReadHook (ap=0x31d23d80) at /work/DeviceLockProject/DeviceLockIOKitDriver/VifVNodeHook.cpp:219
                 #180 0x0035717e in VNOP_READ (vp=0x7d85cb8, uio=0x31d23e60, ioflag=2048, ctx=0x31d23f0c) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/kpi_vfs.c:3458
                 #181 0x0034d237 in vn_read (fp=0x6348af0, uio=0x31d23e60, flags=0, ctx=0x31d23f0c) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/vfs/vfs_vnops.c:740
                 #182 0x00515bc0 in fo_read (fp=0x6348af0, uio=0x31d23e60, flags=0, ctx=0x31d23f0c) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/kern_descrip.c:4773
                 #183 0x00544650 in dofileread (ctx=0x31d23f0c, fp=0x6348af0, bufp=4299495856, nbyte=512, offset=-1, flags=0, retval=0x65a4ae4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern sys_generic.c:353
                 #184 0x005441a3 in read_nocancel (p=0x57faa80, uap=0x64669e8, retval=0x65a4ae4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/sys_generic.c:198
                 #185 0x005440ec in read (p=0x57faa80, uap=0x64669e8, retval=0x65a4ae4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/kern/sys_generic.c:181
                 #186 0x005b19b5 in unix_syscall64 (state=0x64669e4) at /work/Mac_OS_X_kernel/10_6_4/xnu-1504.7.4/bsd/dev/i386/systemcalls.c:365
                 */
                // so as you see at the frame 169 the specfs's(redirection from devfs) strategy routine should be called but a vnode is not
                // provided here
                //
                newEntry->setOriginalVopAsNotHooked( indx );
                continue;
            }
            
            //
            // save the original function before changing it
            //
            newEntry->setOriginalVop( indx, original );
            
            //
            // change to the hooking function
            //
            bytes = FltWriteWiredSrcToWiredDst( (vm_offset_t)&hook,
                                               (vm_offset_t)v_op + offsetDescEntry->offset,
                                               sizeof( VOPFUNC ) );
            
            assert( sizeof( VOPFUNC ) == bytes );
            
        }// end for
        
        
    __exit_with_lock:;
        
    }// end of the lock
    FltVnodeHooksHashTable::sVnodeHooksHashTable->UnLockExclusive();
    
    assert( newEntry );
    
    //
    // the entry has been referenced by AddEntry() or was not added at all,
    // in any case it must be released
    //
    newEntry->release();
    
    if( kIOReturnSuccess == RC )
        *isVopHooked = true;
    
    return RC;
}

//--------------------------------------------------------------------

void
FltUnHookVnodeVop(
                  __inout vnode_t vnode
                  )
{
    VOPFUNC*  v_op;
    
    assert( preemption_enabled() );
    assert( NULL != FltVnodeHooksHashTable::sVnodeHooksHashTable );
    
    //
    // see FltHookVnodeVop(), we are not hooking all vnodes
    //
    if( VREG != vnode_vtype( vnode ) &&
       VDIR != vnode_vtype( vnode ) &&
       VBLK != vnode_vtype( vnode ) &&
       VCHR != vnode_vtype( vnode ) ){
        
        //
        // we are interested only in disk related vnodes
        //
        return;
    }
    
    if( (VOPFUNC*)VIF_VOP_UNKNOWN_OFFSET == FltGetVnodeOpVector( vnode ) || !FltVnodeHooksHashTable::sVnodeHooksHashTable )
        return;
    
    //
    // get the v_op vector's address
    //
    v_op = FltGetVnodeOpVector( vnode );
    
    assert( NULL != v_op );
    assert( FltVirtToPhys( (vm_offset_t)v_op ) );
    
    FltVnodeHookEntry*  existingEntry = NULL;
    
    FltVnodeHooksHashTable::sVnodeHooksHashTable->LockShared();
    {// start of the lock
        existingEntry = FltVnodeHooksHashTable::sVnodeHooksHashTable->RetrieveEntry( v_op, true );
    }// end of the lock
    FltVnodeHooksHashTable::sVnodeHooksHashTable->UnLockShared();
    
    //
    // UnHook is called only when VifIOVnode is being deleted,
    // all VifIOVnodes are for hooked vnodes
    //
    assert( existingEntry );
    
    if( !existingEntry )
        return;
    
    //
    // fast check
    //
    if( 0x1 != existingEntry->decrementVnodeCounter() ){
        
        existingEntry->release();
        return;
    }
    
    FltVnodeHooksHashTable::sVnodeHooksHashTable->LockExclusive();
    {// start of the lock
        
        //
        // the counter might have been incremented as the entry has not been removed
        // from the hash table
        //
        if( 0x0 != existingEntry->getVnodeCounter() )
            goto __exit_with_lock;
        
        existingEntry->release();
        existingEntry = NULL;
        
        //
        // remove the entry for the vnode operations, the returned entry is referenced,
        // the returned entry might be NULL as there is a time slot when the race is possible
        // i.e. when the shared lock is released and the exclusive lock is not yet acquired
        // the concurrent thread might sneak in and remove the entry from the hash table and unhook
        // the vnode operations vector
        //
        existingEntry = FltVnodeHooksHashTable::sVnodeHooksHashTable->RemoveEntry( v_op );
        if( !existingEntry )
            goto __exit_with_lock;
        
        assert( 0x0 == existingEntry->getVnodeCounter() );
        
#if defined( DBG )
        assert( 0x0 != gVifVNodeHookCount );
        OSDecrementAtomic( &gVifVNodeHookCount );
#endif//DBG
        
        //
        // iterate through the registered hooks
        //
        for( int i = 0x0; NULL != gVifVnodeVopHookEntries[ i ].opve_op; ++i ){
            
            FltVnodeOpvOffsetDesc*   offsetDescEntry;
            vnodeopv_entry_desc*     enumDescEntry;
            
            offsetDescEntry = FltRetriveVnodeOpvOffsetDescByVnodeOpDesc( gVifVnodeVopHookEntries[ i ].opve_op );
            assert( offsetDescEntry );
            if( !offsetDescEntry ){
                
                DBG_PRINT_ERROR( ( "FltHookVnodeVop()->FltRetriveVnodeOpvOffsetDescByVnodeOpDesc(%d) failed\n", i ) );
                continue;
            }
            
            enumDescEntry = VifRetriveVnodeOpvEntryDescByVnodeOpDesc( gVifVnodeVopEnumEntries, offsetDescEntry->opve_op );
            assert( enumDescEntry );
            if( !enumDescEntry ){
                
                DBG_PRINT_ERROR( ( "FltHookVnodeVop()->VifRetriveVnodeOpvEntryDescByVnodeOpDesc(%d) failed\n", i ) );
                continue;
            }
            
            assert( offsetDescEntry->opve_op == enumDescEntry->opve_op );
            assert( VIF_VOP_UNKNOWN_OFFSET != offsetDescEntry->offset );
            assert( gVifVnodeVopHookEntries[ i ].opve_impl );
            
            VOPFUNC       original;
            FltVopEnum    indx;
            unsigned int  bytes;
            
            indx = *(FltVopEnum*)(&enumDescEntry->opve_impl);// just to calm the compiler
            
            //
            // the hooking could have been deliberately skipped, as in the case of the
            // strategic routine for specfs
            //
            if( !existingEntry->isHooked( indx ) )
                continue;
            
            original = existingEntry->getOrignalVop( indx );
            assert( original );
            
            //
            // restore to the original function
            //
            bytes = FltWriteWiredSrcToWiredDst( (vm_offset_t)&original,
                                               (vm_offset_t)v_op + offsetDescEntry->offset,
                                               sizeof( VOPFUNC ) );
            
            assert( sizeof( VOPFUNC ) == bytes );
            
        }// end for
        
        
    __exit_with_lock:;
        
    }// end of the lock
    FltVnodeHooksHashTable::sVnodeHooksHashTable->UnLockExclusive();
    
    //
    // the entry returned by RemoveEntry() or RetrieveEntry() is referenced
    //
    if( existingEntry )
        existingEntry->release();
}

//--------------------------------------------------------------------

FltVnodeHooksHashTable*
FltVnodeHooksHashTable::withSize( int size, bool non_block )
{
    FltVnodeHooksHashTable* vNodeHooksHashTable;
    
    assert( preemption_enabled() );
    
    vNodeHooksHashTable = new FltVnodeHooksHashTable();
    assert( vNodeHooksHashTable );
    if( !vNodeHooksHashTable )
        return NULL;
    
    vNodeHooksHashTable->RWLock = IORWLockAlloc();
    assert( vNodeHooksHashTable->RWLock );
    if( !vNodeHooksHashTable->RWLock ){
        
        delete vNodeHooksHashTable;
        return NULL;
    }
    
    vNodeHooksHashTable->HashTable = ght_create( size, non_block );
    assert( vNodeHooksHashTable->HashTable );
    if( !vNodeHooksHashTable->HashTable ){
        
        IORWLockFree( vNodeHooksHashTable->RWLock );
        vNodeHooksHashTable->RWLock = NULL;
        
        delete vNodeHooksHashTable;
        return NULL;
    }
    
    return vNodeHooksHashTable;
}

//--------------------------------------------------------------------

#define super OSObject

OSDefineMetaClassAndStructors( FltVnodeHookEntry, OSObject )

VOPFUNC  FltVnodeHookEntry::vopNotHooked = (VOPFUNC)(-1);

//--------------------------------------------------------------------

bool FltVnodeHookEntry::init()
{
    if( !super::init() ){
        
        assert( !"something awful happened with the system as OSObject::init failed" );
        return false;
    }
    
    this->vNodeCounter = 0x0;
    bzero( this->origVop, sizeof( this->origVop ) );
    
#if defined( DBG )
    this->inHash       = false;
#endif
    
    return true;
};


void FltVnodeHookEntry::free()
{
    assert( 0x0 == this->vNodeCounter );
#if defined( DBG )
    assert( !this->inHash );
#endif
    
    super::free();
};

//--------------------------------------------------------------------

bool
FltVnodeHooksHashTable::CreateStaticTableWithSize( int size, bool non_block )
{
    assert( !FltVnodeHooksHashTable::sVnodeHooksHashTable );
    
    FltVnodeHooksHashTable::sVnodeHooksHashTable = FltVnodeHooksHashTable::withSize( size, non_block );
    assert( FltVnodeHooksHashTable::sVnodeHooksHashTable );
    
    return ( NULL != FltVnodeHooksHashTable::sVnodeHooksHashTable );
}

void
FltVnodeHooksHashTable::DeleteStaticTable()
{
    if( NULL != FltVnodeHooksHashTable::sVnodeHooksHashTable ){
        
        FltVnodeHooksHashTable::sVnodeHooksHashTable->free();
        
        delete FltVnodeHooksHashTable::sVnodeHooksHashTable;
        
        FltVnodeHooksHashTable::sVnodeHooksHashTable = NULL;
    }// end if
}

//--------------------------------------------------------------------

void
FltVnodeHooksHashTable::free()
{
    ght_hash_table_t* p_table;
    ght_iterator_t iterator;
    void *p_key;
    ght_hash_entry_t *p_e;
    
    assert( preemption_enabled() );
    
    p_table = this->HashTable;
    assert( p_table );
    if( !p_table )
        return;
    
    this->HashTable = NULL;
    
    for( p_e = (ght_hash_entry_t*)ght_first( p_table, &iterator, (const void**)&p_key );
        NULL != p_e;
        p_e = (ght_hash_entry_t*)ght_next( p_table, &iterator, (const void**)&p_key ) ){
        
        assert( !"Non emprty hash!" );
        DBG_PRINT_ERROR( ("FltVnodeHooksHashTable::free() found an entry for an object(0x%p)\n", *(void**)p_key ) );
        
        FltVnodeHookEntry* entry = (FltVnodeHookEntry*)p_e->p_data;
        assert( entry );
        entry->release();
        
        p_table->fn_free( p_e, p_e->size );
    }
    
    ght_finalize( p_table );
    
    IORWLockFree( this->RWLock );
    this->RWLock = NULL;
    
}

//--------------------------------------------------------------------

bool
FltVnodeHooksHashTable::AddEntry(
                                 __in VOPFUNC* v_op,
                                 __in FltVnodeHookEntry* entry
                                 )
/*
 the caller must allocate space for the entry and
 free it only after removing the entry from the hash,
 the entry is referenced, so a caller can release it
 */
{
    GHT_STATUS_CODE RC;
    
    RC = ght_insert( this->HashTable, entry, sizeof( v_op ), &v_op );
    assert( GHT_OK == RC );
    if( GHT_OK != RC ){
        
        DBG_PRINT_ERROR( ( "FltVnodeHooksHashTable::AddEntry->ght_insert( 0x%p, 0x%p ) failed RC = 0x%X\n",
                          (void*)v_op, (void*)entry, RC ) );
    } else {
        
        entry->retain();
#if defined( DBG )
        entry->inHash = true;
#endif//DBG
    }
    
    return ( GHT_OK == RC );
}

//--------------------------------------------------------------------

FltVnodeHookEntry*
FltVnodeHooksHashTable::RemoveEntry(
                                    __in VOPFUNC* v_op
                                    )
/*
 the returned entry is referenced!
 */
{
    FltVnodeHookEntry* entry;
    
    //
    // the entry was refernced when was added to the hash table
    //
    entry = (FltVnodeHookEntry*)ght_remove( this->HashTable, sizeof( v_op ), &v_op );
    
#if defined( DBG )
    if( entry ){
        
        assert( true == entry->inHash );
        entry->inHash = false;
        
    }
#endif//DBG
    
    return entry;
}

//--------------------------------------------------------------------

FltVnodeHookEntry*
FltVnodeHooksHashTable::RetrieveEntry(
                                      __in VOPFUNC* v_op,
                                      __in bool reference
                                      )
/*
 the returned entry is referenced if the refernce's value is "true"
 */
{
    FltVnodeHookEntry* entry;
    
    entry = (FltVnodeHookEntry*)ght_get( this->HashTable, sizeof( v_op ), &v_op );
    
    if( entry && reference )
        entry->retain();
    
    return entry;
}

//--------------------------------------------------------------------
