/* 
 * Copyright (c) 2010 Slava Imameev. All rights reserved.
 */

#ifndef _VIFIOVNODE_H
#define _VIFIOVNODE_H

#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSString.h>
#include <IOKit/assert.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include "Common.h"
//#include "VifIOService.h"
#include "VifSparseFile.h"
//#include "VifServiceProtection.h"

//
// the class is more the structure, the inheritance from the OSObject is used
// to use its reference count facility
//
class VifIOVnode : public OSObject
{    
    OSDeclareDefaultStructors( VifIOVnode )
    
protected:
    
    IORecursiveLock*  RecursiveLock;
    
#if defined(DBG)
    int               exclusiveCounter;
    thread_t          ExclusiveThread;
#endif//DBG
    
    //
    // a file name ( should be UTF-8 but depends on FSD capabilities ),
    // the field is protected by the lock, the name oject can be changed
    // during the object lifetime! Use getNameRef() to get a read access to this field.
    //
    OSString*         nameStr;
    
    //
    // used as a placeholder when the name is unknown
    //
    static OSString*  EmptyName;
    
#if defined( DBG )
    static SInt32     VifIOVnodesCount;
#endif//DBG
    
    virtual void free();
    virtual bool init();
    
public:
    
    typedef enum _VnodeType{
        kVnodeType_Native = 0x0,
        kVnodeType_CoveringFSD = 0x1
    } VnodeType;
    
    typedef enum _VnodeOperation{
        kVnodeOp_Unknown = 0x0,
        kVnodeOp_Open,
        kVnodeOp_Close,
        kVnodeOp_Inactive,
        kVnodeOp_Reclaim
    } VnodeOperation;
    
    //
    // called once at the driver initialization
    //
    static bool InitVnodeSubsystem();
    
    //
    // called at driver unload ( which is never happened )
    //
    static void UninitVnodeSubsystem();
    
    static VifIOVnode* withBSDVnode( __in vnode_t vnode );
    
    const char* vnodeTypeCStrNoCopy();
    
    void logVnodeOperation( __in VifIOVnode::VnodeOperation op );
    
    void LockShared();
    void UnLockShared();
    
    void LockExclusive();
    void UnLockExclusive();
    
    //
    // used to queue vnodes having the same hash key
    //
    CIRCLEQ_ENTRY( VifIOVnode )  listEntry;
    
    //
    // a BSD vnode related with this VifIOVnode,
    // must not be referenced if the vnode
    // might have been reclaimed or can be reclaimed
    // concurrently
    //
    vnode_t       vnode;
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    //
    // coveringVnode(or coveredVnode) might be NULL, 
    // coveredVnode has nonzero user count and zero iocount if there is no ongoing io operation on the vnode,
    // the lifetime for coveringVnode is defined by the system and applications,
    // for reference see vflush() and vnode_drain() that are called on a volume unmount
    //
    union{
       
        //
        // this is a vnode which covers the vnode, used for kVnodeType_Native type
        //
        vnode_t       coveringVnode;
        
        //
        // this is a vnode which is covered by vnode, used for kVnodeType_CoveringFSD type
        //
        vnode_t       coveredVnode;
        
    };
    
    //
    // a sparse file that contains not flushed data, might be NULL
    //
    VifSparseFile*  sparseFile;
    
    //
    // a head of the list used to block an application on open and create requests
    // waiting for the responce from the service
    //
    LIST_ENTRY      cawlWaitListHead;
    
    //
    // the file size as reported to UBC, valid only for kVnodeType_CoveringFSD objects
    //
    off_t         fileDataSize;
    
#endif//#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    enum vtype    v_type;
    
    //
    // defines the type of vnode as  defined by this driver,
    // currently there are two type - Native and Covering
    //
    VnodeType     dldVnodeType;
    
    //int           nameBuffLength;// including the terminating zero
    
    //
    // an ID used by a user client
    //
    UInt64        vnodeID;
    
    //
    // used for the vnodes and sparse files creation synchronization,
    // ISOALTION notification lists access,
    // a spin lock is used as it is impossible to use IORecursiveLock
    // with assert_wait because of possible reschedlung after or inside
    // assert_wait
    //
    IOSimpleLock*      spinLock;
    
    //
    // used as a placeholder when the name is unknown
    //
    static const OSSymbol*   UnknownProcessName;
    
    //
    // audit data, pay attention that audit data is valid only after KAUTH_FILEOP_OPEN returned
    //
    struct {
        
        
        //
        // if true the rights should be converted to encrypted ones
        //
        bool                  auditAsEncrypted;
        
        struct{
            
            //
            // a name of the process that created the vnode,
            // as this is an OSSymbol only one instance is created
            // for every process name, this is a refrerenced object
            // set to UnknownProcessName on object creation,
            // never access this field directly, use getProcessAuditNameRef(),
            // the field is protected by the spin lock
            //
            const OSSymbol*          processName;
            
            //
            // (-1) is an invalid value set during the structure initialization
            //
            pid_t              pid;
            
        } process;
        
        
        struct {
            
            //
            // the following user IDs fields are present in any object,
            // it would be a good idea to redesign this as an index in
            // the array of user IDs as the number of users is several
            // order of magnitude less than the number of vnodes
            //
            // a user's GUID, zeroed if unknown
            //
            guid_t             guid;
            
            //
            // a user's UID, (-1) if unknown as 0 is a superuser ( root )
            //
            uid_t              uid;
            
            //
            // a user's GID, zeroed if unknown
            //
            gid_t              gid;
            
        }  userID;
    } auditData;
    
    //
    // the set of flags, protected by the spinLock(!)
    // do not set the flags w/o the spin lock being held as this
    // is not an atomic operation, but you can read w/o 
    // holding the spin lock
    //
    struct{
        
        //
        // a vnode is created by DL
        //
        unsigned int internal:0x1;
        
        //
        // as returned by vnode_issystem()
        //
        unsigned int system:0x1;
        
        //
        // as returned by vnode_islnk()
        //
        unsigned int link:0x1;
        
        //
        // as returned by vnode_isswap()
        //
        unsigned int swap:0x1;
        
        //
        // as returned by vnode_isdir()
        //
        unsigned int dir:0x1;
        
        //
        // 0x1 if the vnode is for a direct disk open
        // either through the DIR where a FS(i.e. /Volumes/NO NAME ) is mounted
        // or through a /dev/XXX object open
        //
        unsigned int directDiskOpen:0x1;
        
        //
        // a FS root opening
        //
        unsigned int fsRoot:0x1;
        
        //
        // 0x1 if the file supports a virtual disk ( e.g. FileVault )
        //
        unsigned int virtualDiskFile:0x1;
        
        //
        // 0x1 means that vnode's operations have been hooked
        // so the hooked count was bumped
        //
        unsigned int vopHooked:0x1;
        
        //
        // 0x1 if the vnode should be under the ISOALTION control,
        // but this flag doesn't mean that all structures
        // related to ISOALTION have been initialized for the vnode,
        // the flag is normally set by defineStatusForIsolation(),
        // so this flag marks an intention
        //
        unsigned int isolationOn:0x1;
        
        //
        // 0x1 if the vnode has been reclaimed, so the vnode_t is invalid
        //
        unsigned int reclaimed:0x1;
                
        //
        // if 0x1 a concurrent thread is initializing a sparse file
        //
        unsigned int sparseFileIsBeingCreated:0x1;
        
        //
        // 0x1 if the sparse file creation has failed
        //
        unsigned int sparseFileCreationHasFailed:0x1;
        
        //
        // 0x1 if the covering vnode is being created
        //
        unsigned int coveringVnodeIsBeingCreated:0x1;
        
        //
        // if 0x1 then the covering vnode has been reclaimed,
        // this is an informative flag used only for debug,
        // the covering vnode can be recreated several times
        // for the same vnode
        //
        unsigned int coveringVnodeReclaimed:0x1;
        

    } flags;
    
    //
    // if not 0x0 the reclaim must be delayed because of ongoing vnode referencing
    //
    SInt32 delayReclaimCount;
    
    //
    // a counter is used to keep track of the service security settings changes
    // and to update protectedFile object
    //
    SInt32 serviceProtectionSettingsSequenceCounter;
    
    //
    // returns a referenced object name, a caller must dereference
    //
    OSString* getNameRef();
    
    //
    // sets a new vnode name
    //
    void setName( __in OSString* name );
    
    //
    // called from KAUTH callback, updates the name if the previous one is a dummy one,
    // must not be called if the valifity of the vnode can't be guaranted!
    //
    void updateNameToValid();
    
    //
    // returns true if the name has been updated to a valid one
    //
    bool isNameVaid();
    
    //
    // set vnode's ISOALTION status
    //
    void defineStatusForIsolation( __in vfs_context_t vfsContext, __in_opt const char* namepath );
    
    //
    // set usr's ID , this is a yser who opened the file
    //
    void setUserID( __in kauth_cred_t    credential );
    
    void setProcess( __in pid_t pid );
    
    //
    // returns a referenced pointer to the process name for audit, never return NULL, a caller must release the reference
    //
    const OSSymbol* getProcessAuditNameRef();
    
    void setAuditData( __in kauth_cred_t credential, __in pid_t pid, __in bool auditAsEncrypted )
    {
        this->setUserID( credential );
        this->setProcess( pid );
        this->auditData.auditAsEncrypted = auditAsEncrypted;
    };
    
    void prepareAuditDataForReuse()
    {
        this->auditData.process.pid = (-1);
        this->auditData.userID.uid  = (-1);
    }
    
    //
    // returns a referenced vnode returns NULL if the vnode is being terminated or has been terminated,
    // a caller must call vnode_put() for returned vnode
    //
    vnode_t getReferencedVnode();
    
    void prepareForReclaiming();
    
    //
    // onclu ISOALTION controlled covered vnodes are reported to the service for control as
    // covered vnode IO is triggered by the isolation engine itself
    //
    bool isControlledByIsolationFilter() { return (0x1 == this->flags.isolationOn && kVnodeType_CoveringFSD == this->dldVnodeType);}
    
#ifdef _VIF_MACOSX_VFS_ISOLATION
    
    //
    // a caller must release the returned object
    //
    VifSparseFile* getSparseFileRef();
#endif // _VIF_MACOSX_VFS_ISOLATION
    
#if DBG
    VnodeOperation  vnodeOperationsLog[8];
#endif//DBG
};


extern
errno_t
VifVnodeSetsize(vnode_t vp, off_t size, int ioflag, vfs_context_t ctx);

#endif//_VIFIOVNODE_H
