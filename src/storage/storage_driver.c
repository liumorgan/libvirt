/*
 * storage_driver.c: core driver for storage APIs
 *
 * Copyright (C) 2006-2015 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <fcntl.h>

#if HAVE_PWD_H
# include <pwd.h>
#endif
#include <errno.h>
#include <string.h>

#include "virerror.h"
#include "datatypes.h"
#include "driver.h"
#include "storage_driver.h"
#include "storage_conf.h"
#include "storage_event.h"
#include "viralloc.h"
#include "storage_backend.h"
#include "virlog.h"
#include "virfile.h"
#include "virfdstream.h"
#include "configmake.h"
#include "virsecret.h"
#include "virstring.h"
#include "viraccessapicheck.h"
//#include "dirname.h"
#include "storage_util.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE

VIR_LOG_INIT("storage.storage_driver");

static virStorageDriverStatePtr driver;

static int storageStateCleanup(void);

typedef struct _virStorageVolStreamInfo virStorageVolStreamInfo;
typedef virStorageVolStreamInfo *virStorageVolStreamInfoPtr;
struct _virStorageVolStreamInfo {
    char *pool_name;
    char *vol_path;
};

static void storageDriverLock(void)
{
    virMutexLock(&driver->lock);
}
static void storageDriverUnlock(void)
{
    virMutexUnlock(&driver->lock);
}


/**
 * virStoragePoolUpdateInactive:
 * @poolptr: pointer to a variable holding the pool object pointer
 *
 * This function is supposed to be called after a pool becomes inactive. The
 * function switches to the new config object for persistent pools. Inactive
 * pools are removed.
 */
static void
virStoragePoolUpdateInactive(virStoragePoolObjPtr *objptr)
{
    virStoragePoolObjPtr obj = *objptr;

    if (!virStoragePoolObjGetConfigFile(obj)) {
        virStoragePoolObjRemove(&driver->pools, obj);
        *objptr = NULL;
    } else if (virStoragePoolObjGetNewDef(obj)) {
        virStoragePoolObjDefUseNewDef(obj);
    }
}


static void
storagePoolUpdateState(virStoragePoolObjPtr obj)
{
    virStoragePoolDefPtr def = virStoragePoolObjGetDef(obj);
    bool active = false;
    virStorageBackendPtr backend;
    char *stateFile;

    if (!(stateFile = virFileBuildPath(driver->stateDir, def->name, ".xml")))
        goto cleanup;

    if ((backend = virStorageBackendForType(def->type)) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Missing backend %d"), def->type);
        goto cleanup;
    }

    /* Backends which do not support 'checkPool' are considered
     * inactive by default. */
    if (backend->checkPool &&
        backend->checkPool(obj, &active) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to initialize storage pool '%s': %s"),
                       def->name, virGetLastErrorMessage());
        active = false;
    }

    /* We can pass NULL as connection, most backends do not use
     * it anyway, but if they do and fail, we want to log error and
     * continue with other pools.
     */
    if (active) {
        virStoragePoolObjClearVols(obj);
        if (backend->refreshPool(NULL, obj) < 0) {
            if (backend->stopPool)
                backend->stopPool(NULL, obj);
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Failed to restart storage pool '%s': %s"),
                           def->name, virGetLastErrorMessage());
            active = false;
        }
    }

    virStoragePoolObjSetActive(obj, active);

    if (!virStoragePoolObjIsActive(obj))
        virStoragePoolUpdateInactive(&obj);

 cleanup:
    if (!active && stateFile)
        ignore_value(unlink(stateFile));
    VIR_FREE(stateFile);

    return;
}

static void
storagePoolUpdateAllState(void)
{
    size_t i;

    for (i = 0; i < driver->pools.count; i++) {
        virStoragePoolObjPtr obj = driver->pools.objs[i];

        virStoragePoolObjLock(obj);
        storagePoolUpdateState(obj);
        virStoragePoolObjUnlock(obj);
    }
}

static void
storageDriverAutostart(void)
{
    size_t i;
    virConnectPtr conn = NULL;

    /* XXX Remove hardcoding of QEMU URI */
    if (driver->privileged)
        conn = virConnectOpen("qemu:///system");
    else
        conn = virConnectOpen("qemu:///session");
    /* Ignoring NULL conn - let backends decide */

    for (i = 0; i < driver->pools.count; i++) {
        virStoragePoolObjPtr obj = driver->pools.objs[i];
        virStoragePoolDefPtr def = virStoragePoolObjGetDef(obj);
        virStorageBackendPtr backend;
        bool started = false;

        virStoragePoolObjLock(obj);
        if ((backend = virStorageBackendForType(def->type)) == NULL) {
            virStoragePoolObjUnlock(obj);
            continue;
        }

        if (virStoragePoolObjIsAutostart(obj) &&
            !virStoragePoolObjIsActive(obj)) {
            if (backend->startPool &&
                backend->startPool(conn, obj) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Failed to autostart storage pool '%s': %s"),
                               def->name, virGetLastErrorMessage());
                virStoragePoolObjUnlock(obj);
                continue;
            }
            started = true;
        }

        if (started) {
            char *stateFile;

            virStoragePoolObjClearVols(obj);
            stateFile = virFileBuildPath(driver->stateDir, def->name, ".xml");
            if (!stateFile ||
                virStoragePoolSaveState(stateFile, def) < 0 ||
                backend->refreshPool(conn, obj) < 0) {
                if (stateFile)
                    unlink(stateFile);
                if (backend->stopPool)
                    backend->stopPool(conn, obj);
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Failed to autostart storage pool '%s': %s"),
                               def->name, virGetLastErrorMessage());
            } else {
                virStoragePoolObjSetActive(obj, true);
            }
            VIR_FREE(stateFile);
        }
        virStoragePoolObjUnlock(obj);
    }

    virObjectUnref(conn);
}

/**
 * virStorageStartup:
 *
 * Initialization function for the Storage Driver
 */
static int
storageStateInitialize(bool privileged,
                       virStateInhibitCallback callback ATTRIBUTE_UNUSED,
                       void *opaque ATTRIBUTE_UNUSED)
{
    int ret = -1;
    char *configdir = NULL;
    char *rundir = NULL;

    if (VIR_ALLOC(driver) < 0)
        return ret;

    if (virMutexInit(&driver->lock) < 0) {
        VIR_FREE(driver);
        return ret;
    }
    storageDriverLock();

    if (privileged) {
        if (VIR_STRDUP(driver->configDir,
                       SYSCONFDIR "/libvirt/storage") < 0 ||
            VIR_STRDUP(driver->autostartDir,
                       SYSCONFDIR "/libvirt/storage/autostart") < 0 ||
            VIR_STRDUP(driver->stateDir,
                       LOCALSTATEDIR "/run/libvirt/storage") < 0)
            goto error;
    } else {
        configdir = virGetUserConfigDirectory();
        rundir = virGetUserRuntimeDirectory();
        if (!(configdir && rundir))
            goto error;

        if ((virAsprintf(&driver->configDir,
                        "%s/storage", configdir) < 0) ||
            (virAsprintf(&driver->autostartDir,
                        "%s/storage/autostart", configdir) < 0) ||
            (virAsprintf(&driver->stateDir,
                         "%s/storage/run", rundir) < 0))
            goto error;
    }
    driver->privileged = privileged;

    if (virFileMakePath(driver->stateDir) < 0) {
        virReportError(errno,
                       _("cannot create directory %s"),
                       driver->stateDir);
        goto error;
    }

    if (virStoragePoolObjLoadAllState(&driver->pools,
                                      driver->stateDir) < 0)
        goto error;

    if (virStoragePoolObjLoadAllConfigs(&driver->pools,
                                        driver->configDir,
                                        driver->autostartDir) < 0)
        goto error;

    storagePoolUpdateAllState();

    driver->storageEventState = virObjectEventStateNew();

    storageDriverUnlock();

    ret = 0;
 cleanup:
    VIR_FREE(configdir);
    VIR_FREE(rundir);
    return ret;

 error:
    storageDriverUnlock();
    storageStateCleanup();
    goto cleanup;
}

/**
 * storageStateAutoStart:
 *
 * Function to auto start the storage driver
 */
static void
storageStateAutoStart(void)
{
    if (!driver)
        return;

    storageDriverLock();
    storageDriverAutostart();
    storageDriverUnlock();
}

/**
 * storageStateReload:
 *
 * Function to restart the storage driver, it will recheck the configuration
 * files and update its state
 */
static int
storageStateReload(void)
{
    if (!driver)
        return -1;

    storageDriverLock();
    virStoragePoolObjLoadAllState(&driver->pools,
                                  driver->stateDir);
    virStoragePoolObjLoadAllConfigs(&driver->pools,
                                    driver->configDir,
                                    driver->autostartDir);
    storageDriverAutostart();
    storageDriverUnlock();

    return 0;
}


/**
 * storageStateCleanup
 *
 * Shutdown the storage driver, it will stop all active storage pools
 */
static int
storageStateCleanup(void)
{
    if (!driver)
        return -1;

    storageDriverLock();

    virObjectUnref(driver->storageEventState);

    /* free inactive pools */
    virStoragePoolObjListFree(&driver->pools);

    VIR_FREE(driver->configDir);
    VIR_FREE(driver->autostartDir);
    VIR_FREE(driver->stateDir);
    storageDriverUnlock();
    virMutexDestroy(&driver->lock);
    VIR_FREE(driver);

    return 0;
}


static virStoragePoolObjPtr
storagePoolObjFindByUUID(const unsigned char *uuid,
                         const char *name)
{
    virStoragePoolObjPtr obj;
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    if (!(obj = virStoragePoolObjFindByUUID(&driver->pools, uuid))) {
        virUUIDFormat(uuid, uuidstr);
        if (name)
            virReportError(VIR_ERR_NO_STORAGE_POOL,
                           _("no storage pool with matching uuid '%s' (%s)"),
                           uuidstr, name);
        else
            virReportError(VIR_ERR_NO_STORAGE_POOL,
                           _("no storage pool with matching uuid '%s'"),
                           uuidstr);
    }

    return obj;
}


static virStoragePoolObjPtr
virStoragePoolObjFromStoragePool(virStoragePoolPtr pool)
{
    virStoragePoolObjPtr ret;

    storageDriverLock();
    ret = storagePoolObjFindByUUID(pool->uuid, pool->name);
    storageDriverUnlock();

    return ret;
}


static virStoragePoolObjPtr
storagePoolObjFindByName(const char *name)
{
    virStoragePoolObjPtr obj;

    storageDriverLock();
    if (!(obj = virStoragePoolObjFindByName(&driver->pools, name)))
        virReportError(VIR_ERR_NO_STORAGE_POOL,
                       _("no storage pool with matching name '%s'"), name);
    storageDriverUnlock();

    return obj;
}


static virStoragePoolPtr
storagePoolLookupByUUID(virConnectPtr conn,
                        const unsigned char *uuid)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStoragePoolPtr pool = NULL;

    storageDriverLock();
    obj = storagePoolObjFindByUUID(uuid, NULL);
    storageDriverUnlock();
    if (!obj)
        return NULL;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolLookupByUUIDEnsureACL(conn, def) < 0)
        goto cleanup;

    pool = virGetStoragePool(conn, def->name, def->uuid, NULL, NULL);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return pool;
}

static virStoragePoolPtr
storagePoolLookupByName(virConnectPtr conn,
                        const char *name)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStoragePoolPtr pool = NULL;

    if (!(obj = storagePoolObjFindByName(name)))
        return NULL;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolLookupByNameEnsureACL(conn, def) < 0)
        goto cleanup;

    pool = virGetStoragePool(conn, def->name, def->uuid, NULL, NULL);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return pool;
}

static virStoragePoolPtr
storagePoolLookupByVolume(virStorageVolPtr vol)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStoragePoolPtr pool = NULL;

    if (!(obj = storagePoolObjFindByName(vol->pool)))
        return NULL;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolLookupByVolumeEnsureACL(vol->conn, def) < 0)
        goto cleanup;

    pool = virGetStoragePool(vol->conn, def->name, def->uuid, NULL, NULL);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return pool;
}

static int
storageConnectNumOfStoragePools(virConnectPtr conn)
{
    int nactive = 0;

    if (virConnectNumOfStoragePoolsEnsureACL(conn) < 0)
        return -1;

    storageDriverLock();
    nactive = virStoragePoolObjNumOfStoragePools(&driver->pools, conn, true,
                                                 virConnectNumOfStoragePoolsCheckACL);
    storageDriverUnlock();

    return nactive;
}


static int
storageConnectListStoragePools(virConnectPtr conn,
                               char **const names,
                               int maxnames)
{
    int got = 0;

    if (virConnectListStoragePoolsEnsureACL(conn) < 0)
        return -1;

    storageDriverLock();
    got = virStoragePoolObjGetNames(&driver->pools, conn, true,
                                    virConnectListStoragePoolsCheckACL,
                                    names, maxnames);
    storageDriverUnlock();
    return got;
}

static int
storageConnectNumOfDefinedStoragePools(virConnectPtr conn)
{
    int nactive = 0;

    if (virConnectNumOfDefinedStoragePoolsEnsureACL(conn) < 0)
        return -1;

    storageDriverLock();
    nactive = virStoragePoolObjNumOfStoragePools(&driver->pools, conn, false,
                                                 virConnectNumOfDefinedStoragePoolsCheckACL);
    storageDriverUnlock();

    return nactive;
}


static int
storageConnectListDefinedStoragePools(virConnectPtr conn,
                                      char **const names,
                                      int maxnames)
{
    int got = 0;

    if (virConnectListDefinedStoragePoolsEnsureACL(conn) < 0)
        return -1;

    storageDriverLock();
    got = virStoragePoolObjGetNames(&driver->pools, conn, false,
                                    virConnectListDefinedStoragePoolsCheckACL,
                                    names, maxnames);
    storageDriverUnlock();
    return got;
}

/* This method is required to be re-entrant / thread safe, so
   uses no driver lock */
static char *
storageConnectFindStoragePoolSources(virConnectPtr conn,
                                     const char *type,
                                     const char *srcSpec,
                                     unsigned int flags)
{
    int backend_type;
    virStorageBackendPtr backend;
    char *ret = NULL;

    if (virConnectFindStoragePoolSourcesEnsureACL(conn) < 0)
        return NULL;

    backend_type = virStoragePoolTypeFromString(type);
    if (backend_type < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown storage pool type %s"), type);
        goto cleanup;
    }

    backend = virStorageBackendForType(backend_type);
    if (backend == NULL)
        goto cleanup;

    if (!backend->findPoolSources) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       _("pool type '%s' does not support source "
                         "discovery"), type);
        goto cleanup;
    }

    ret = backend->findPoolSources(conn, srcSpec, flags);

 cleanup:
    return ret;
}


static int
storagePoolIsActive(virStoragePoolPtr pool)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolIsActiveEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    ret = virStoragePoolObjIsActive(obj);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}


static int
storagePoolIsPersistent(virStoragePoolPtr pool)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolIsPersistentEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    ret = virStoragePoolObjGetConfigFile(obj) ? 1 : 0;

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}


static virStoragePoolPtr
storagePoolCreateXML(virConnectPtr conn,
                     const char *xml,
                     unsigned int flags)
{
    virStoragePoolDefPtr newDef;
    virStoragePoolObjPtr obj = NULL;
    virStoragePoolDefPtr def;
    virStoragePoolPtr pool = NULL;
    virStorageBackendPtr backend;
    virObjectEventPtr event = NULL;
    char *stateFile = NULL;
    unsigned int build_flags = 0;

    virCheckFlags(VIR_STORAGE_POOL_CREATE_WITH_BUILD |
                  VIR_STORAGE_POOL_CREATE_WITH_BUILD_OVERWRITE |
                  VIR_STORAGE_POOL_CREATE_WITH_BUILD_NO_OVERWRITE, NULL);

    VIR_EXCLUSIVE_FLAGS_RET(VIR_STORAGE_POOL_BUILD_OVERWRITE,
                            VIR_STORAGE_POOL_BUILD_NO_OVERWRITE, NULL);

    storageDriverLock();
    if (!(newDef = virStoragePoolDefParseString(xml)))
        goto cleanup;

    if (virStoragePoolCreateXMLEnsureACL(conn, newDef) < 0)
        goto cleanup;

    if (virStoragePoolObjIsDuplicate(&driver->pools, newDef, 1) < 0)
        goto cleanup;

    if (virStoragePoolObjSourceFindDuplicate(conn, &driver->pools, newDef) < 0)
        goto cleanup;

    if ((backend = virStorageBackendForType(newDef->type)) == NULL)
        goto cleanup;

    if (!(obj = virStoragePoolObjAssignDef(&driver->pools, newDef)))
        goto cleanup;
    newDef = NULL;
    def = virStoragePoolObjGetDef(obj);

    if (backend->buildPool) {
        if (flags & VIR_STORAGE_POOL_CREATE_WITH_BUILD_OVERWRITE)
            build_flags |= VIR_STORAGE_POOL_BUILD_OVERWRITE;
        else if (flags & VIR_STORAGE_POOL_CREATE_WITH_BUILD_NO_OVERWRITE)
            build_flags |= VIR_STORAGE_POOL_BUILD_NO_OVERWRITE;

        if (build_flags ||
            (flags & VIR_STORAGE_POOL_CREATE_WITH_BUILD)) {
            if (backend->buildPool(conn, obj, build_flags) < 0) {
                virStoragePoolObjRemove(&driver->pools, obj);
                obj = NULL;
                goto cleanup;
            }
        }
    }

    if (backend->startPool &&
        backend->startPool(conn, obj) < 0) {
        virStoragePoolObjRemove(&driver->pools, obj);
        obj = NULL;
        goto cleanup;
    }

    stateFile = virFileBuildPath(driver->stateDir, def->name, ".xml");

    virStoragePoolObjClearVols(obj);
    if (!stateFile || virStoragePoolSaveState(stateFile, def) < 0 ||
        backend->refreshPool(conn, obj) < 0) {
        if (stateFile)
            unlink(stateFile);
        if (backend->stopPool)
            backend->stopPool(conn, obj);
        virStoragePoolObjRemove(&driver->pools, obj);
        obj = NULL;
        goto cleanup;
    }

    event = virStoragePoolEventLifecycleNew(def->name,
                                            def->uuid,
                                            VIR_STORAGE_POOL_EVENT_STARTED,
                                            0);

    VIR_INFO("Creating storage pool '%s'", def->name);
    virStoragePoolObjSetActive(obj, true);

    pool = virGetStoragePool(conn, def->name, def->uuid, NULL, NULL);

 cleanup:
    VIR_FREE(stateFile);
    virStoragePoolDefFree(newDef);
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    return pool;
}

static virStoragePoolPtr
storagePoolDefineXML(virConnectPtr conn,
                     const char *xml,
                     unsigned int flags)
{
    virStoragePoolDefPtr newDef;
    virStoragePoolObjPtr obj = NULL;
    virStoragePoolDefPtr def;
    virStoragePoolPtr pool = NULL;
    virObjectEventPtr event = NULL;

    virCheckFlags(0, NULL);

    storageDriverLock();
    if (!(newDef = virStoragePoolDefParseString(xml)))
        goto cleanup;

    if (virXMLCheckIllegalChars("name", newDef->name, "\n") < 0)
        goto cleanup;

    if (virStoragePoolDefineXMLEnsureACL(conn, newDef) < 0)
        goto cleanup;

    if (virStoragePoolObjIsDuplicate(&driver->pools, newDef, 0) < 0)
        goto cleanup;

    if (virStoragePoolObjSourceFindDuplicate(conn, &driver->pools, newDef) < 0)
        goto cleanup;

    if (virStorageBackendForType(newDef->type) == NULL)
        goto cleanup;

    if (!(obj = virStoragePoolObjAssignDef(&driver->pools, newDef)))
        goto cleanup;
    newDef = NULL;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolObjSaveDef(driver, obj, def) < 0) {
        virStoragePoolObjRemove(&driver->pools, obj);
        obj = NULL;
        goto cleanup;
    }

    event = virStoragePoolEventLifecycleNew(def->name, def->uuid,
                                            VIR_STORAGE_POOL_EVENT_DEFINED,
                                            0);

    VIR_INFO("Defining storage pool '%s'", def->name);
    pool = virGetStoragePool(conn, def->name, def->uuid, NULL, NULL);

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    virStoragePoolDefFree(newDef);
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    return pool;
}

static int
storagePoolUndefine(virStoragePoolPtr pool)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    const char *autostartLink;
    virObjectEventPtr event = NULL;
    int ret = -1;

    storageDriverLock();
    if (!(obj = storagePoolObjFindByUUID(pool->uuid, pool->name)))
        goto cleanup;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolUndefineEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if (virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is still active"),
                       def->name);
        goto cleanup;
    }

    if (virStoragePoolObjGetAsyncjobs(obj) > 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("pool '%s' has asynchronous jobs running."),
                       def->name);
        goto cleanup;
    }

    autostartLink = virStoragePoolObjGetAutostartLink(obj);
    if (virStoragePoolObjDeleteDef(obj) < 0)
        goto cleanup;

    if (autostartLink && unlink(autostartLink) < 0 &&
        errno != ENOENT && errno != ENOTDIR) {
        char ebuf[1024];
        VIR_ERROR(_("Failed to delete autostart link '%s': %s"),
                  autostartLink, virStrerror(errno, ebuf, sizeof(ebuf)));
    }

    event = virStoragePoolEventLifecycleNew(def->name,
                                            def->uuid,
                                            VIR_STORAGE_POOL_EVENT_UNDEFINED,
                                            0);

    VIR_INFO("Undefining storage pool '%s'", def->name);
    virStoragePoolObjRemove(&driver->pools, obj);
    obj = NULL;
    ret = 0;

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    return ret;
}

static int
storagePoolCreate(virStoragePoolPtr pool,
                  unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virObjectEventPtr event = NULL;
    int ret = -1;
    char *stateFile = NULL;
    unsigned int build_flags = 0;

    virCheckFlags(VIR_STORAGE_POOL_CREATE_WITH_BUILD |
                  VIR_STORAGE_POOL_CREATE_WITH_BUILD_OVERWRITE |
                  VIR_STORAGE_POOL_CREATE_WITH_BUILD_NO_OVERWRITE, -1);

    VIR_EXCLUSIVE_FLAGS_RET(VIR_STORAGE_POOL_BUILD_OVERWRITE,
                            VIR_STORAGE_POOL_BUILD_NO_OVERWRITE, -1);

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolCreateEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    if (virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is already active"),
                       def->name);
        goto cleanup;
    }

    if (backend->buildPool) {
        if (flags & VIR_STORAGE_POOL_CREATE_WITH_BUILD_OVERWRITE)
            build_flags |= VIR_STORAGE_POOL_BUILD_OVERWRITE;
        else if (flags & VIR_STORAGE_POOL_CREATE_WITH_BUILD_NO_OVERWRITE)
            build_flags |= VIR_STORAGE_POOL_BUILD_NO_OVERWRITE;

        if (build_flags ||
            (flags & VIR_STORAGE_POOL_CREATE_WITH_BUILD)) {
            if (backend->buildPool(pool->conn, obj, build_flags) < 0)
                goto cleanup;
        }
    }

    VIR_INFO("Starting up storage pool '%s'", def->name);
    if (backend->startPool &&
        backend->startPool(pool->conn, obj) < 0)
        goto cleanup;

    stateFile = virFileBuildPath(driver->stateDir, def->name, ".xml");

    virStoragePoolObjClearVols(obj);
    if (!stateFile || virStoragePoolSaveState(stateFile, def) < 0 ||
        backend->refreshPool(pool->conn, obj) < 0) {
        if (stateFile)
            unlink(stateFile);
        if (backend->stopPool)
            backend->stopPool(pool->conn, obj);
        goto cleanup;
    }

    event = virStoragePoolEventLifecycleNew(def->name,
                                            def->uuid,
                                            VIR_STORAGE_POOL_EVENT_STARTED,
                                            0);

    virStoragePoolObjSetActive(obj, true);
    ret = 0;

 cleanup:
    VIR_FREE(stateFile);
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    if (obj)
        virStoragePoolObjUnlock(obj);
    return ret;
}

static int
storagePoolBuild(virStoragePoolPtr pool,
                 unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virObjectEventPtr event = NULL;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolBuildEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    if (virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is already active"),
                       def->name);
        goto cleanup;
    }

    if (backend->buildPool &&
        backend->buildPool(pool->conn, obj, flags) < 0)
        goto cleanup;

    event = virStoragePoolEventLifecycleNew(def->name,
                                            def->uuid,
                                            VIR_STORAGE_POOL_EVENT_CREATED,
                                            0);

    ret = 0;

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    virStoragePoolObjUnlock(obj);
    return ret;
}


static int
storagePoolDestroy(virStoragePoolPtr pool)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virObjectEventPtr event = NULL;
    char *stateFile = NULL;
    int ret = -1;

    storageDriverLock();
    if (!(obj = storagePoolObjFindByUUID(pool->uuid, pool->name)))
        goto cleanup;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolDestroyEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    VIR_INFO("Destroying storage pool '%s'", def->name);

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    if (virStoragePoolObjGetAsyncjobs(obj) > 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("pool '%s' has asynchronous jobs running."),
                       def->name);
        goto cleanup;
    }

    if (!(stateFile = virFileBuildPath(driver->stateDir, def->name, ".xml")))
        goto cleanup;

    unlink(stateFile);
    VIR_FREE(stateFile);

    if (backend->stopPool &&
        backend->stopPool(pool->conn, obj) < 0)
        goto cleanup;

    virStoragePoolObjClearVols(obj);

    event = virStoragePoolEventLifecycleNew(def->name,
                                            def->uuid,
                                            VIR_STORAGE_POOL_EVENT_STOPPED,
                                            0);

    virStoragePoolObjSetActive(obj, false);

    virStoragePoolUpdateInactive(&obj);

    ret = 0;

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    return ret;
}

static int
storagePoolDelete(virStoragePoolPtr pool,
                  unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virObjectEventPtr event = NULL;
    char *stateFile = NULL;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolDeleteEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    VIR_INFO("Deleting storage pool '%s'", def->name);

    if (virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is still active"),
                       def->name);
        goto cleanup;
    }

    if (virStoragePoolObjGetAsyncjobs(obj) > 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("pool '%s' has asynchronous jobs running."),
                       def->name);
        goto cleanup;
    }

    if (!(stateFile = virFileBuildPath(driver->stateDir, def->name, ".xml")))
        goto cleanup;

    unlink(stateFile);
    VIR_FREE(stateFile);

    if (!backend->deletePool) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       "%s", _("pool does not support pool deletion"));
        goto cleanup;
    }
    if (backend->deletePool(pool->conn, obj, flags) < 0)
        goto cleanup;

    event = virStoragePoolEventLifecycleNew(def->name,
                                            def->uuid,
                                            VIR_STORAGE_POOL_EVENT_DELETED,
                                            0);

    ret = 0;

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    virStoragePoolObjUnlock(obj);
    return ret;
}


static int
storagePoolRefresh(virStoragePoolPtr pool,
                   unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    int ret = -1;
    virObjectEventPtr event = NULL;

    virCheckFlags(0, -1);

    storageDriverLock();
    if (!(obj = storagePoolObjFindByUUID(pool->uuid, pool->name)))
        goto cleanup;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolRefreshEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    if (virStoragePoolObjGetAsyncjobs(obj) > 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("pool '%s' has asynchronous jobs running."),
                       def->name);
        goto cleanup;
    }

    virStoragePoolObjClearVols(obj);
    if (backend->refreshPool(pool->conn, obj) < 0) {
        if (backend->stopPool)
            backend->stopPool(pool->conn, obj);

        event = virStoragePoolEventLifecycleNew(def->name,
                                                def->uuid,
                                                VIR_STORAGE_POOL_EVENT_STOPPED,
                                                0);
        virStoragePoolObjSetActive(obj, false);

        virStoragePoolUpdateInactive(&obj);

        goto cleanup;
    }

    event = virStoragePoolEventRefreshNew(def->name,
                                          def->uuid);
    ret = 0;

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    return ret;
}


static int
storagePoolGetInfo(virStoragePoolPtr pool,
                   virStoragePoolInfoPtr info)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolGetInfoEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if (virStorageBackendForType(def->type) == NULL)
        goto cleanup;

    memset(info, 0, sizeof(virStoragePoolInfo));
    if (virStoragePoolObjIsActive(obj))
        info->state = VIR_STORAGE_POOL_RUNNING;
    else
        info->state = VIR_STORAGE_POOL_INACTIVE;
    info->capacity = def->capacity;
    info->allocation = def->allocation;
    info->available = def->available;
    ret = 0;

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}

static char *
storagePoolGetXMLDesc(virStoragePoolPtr pool,
                      unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStoragePoolDefPtr newDef;
    virStoragePoolDefPtr curDef;
    char *ret = NULL;

    virCheckFlags(VIR_STORAGE_XML_INACTIVE, NULL);

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return NULL;
    def = virStoragePoolObjGetDef(obj);
    newDef = virStoragePoolObjGetNewDef(obj);

    if (virStoragePoolGetXMLDescEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if ((flags & VIR_STORAGE_XML_INACTIVE) && newDef)
        curDef = newDef;
    else
        curDef = def;

    ret = virStoragePoolDefFormat(curDef);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}

static int
storagePoolGetAutostart(virStoragePoolPtr pool,
                        int *autostart)
{
    virStoragePoolObjPtr obj;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;

    if (virStoragePoolGetAutostartEnsureACL(pool->conn,
                                            virStoragePoolObjGetDef(obj)) < 0)
        goto cleanup;

    *autostart = virStoragePoolObjIsAutostart(obj) ? 1 : 0;

    ret = 0;

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}

static int
storagePoolSetAutostart(virStoragePoolPtr pool,
                        int autostart)
{
    virStoragePoolObjPtr obj;
    const char *configFile;
    const char *autostartLink;
    bool new_autostart;
    bool cur_autostart;
    int ret = -1;

    storageDriverLock();
    if (!(obj = storagePoolObjFindByUUID(pool->uuid, pool->name)))
        goto cleanup;

    if (virStoragePoolSetAutostartEnsureACL(pool->conn,
                                            virStoragePoolObjGetDef(obj)) < 0)
        goto cleanup;

    if (!(configFile = virStoragePoolObjGetConfigFile(obj))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("pool has no config file"));
        goto cleanup;
    }

    autostartLink = virStoragePoolObjGetAutostartLink(obj);

    new_autostart = (autostart != 0);
    cur_autostart = virStoragePoolObjIsAutostart(obj);
    if (cur_autostart != new_autostart) {
        if (new_autostart) {
            if (virFileMakePath(driver->autostartDir) < 0) {
                virReportSystemError(errno,
                                     _("cannot create autostart directory %s"),
                                     driver->autostartDir);
                goto cleanup;
            }

            if (symlink(configFile, autostartLink) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create symlink '%s' to '%s'"),
                                     autostartLink, configFile);
                goto cleanup;
            }
        } else {
            if (autostartLink && unlink(autostartLink) < 0 &&
                errno != ENOENT && errno != ENOTDIR) {
                virReportSystemError(errno,
                                     _("Failed to delete symlink '%s'"),
                                     autostartLink);
                goto cleanup;
            }
        }
        virStoragePoolObjSetAutostart(obj, new_autostart);
    }

    ret = 0;

 cleanup:
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    return ret;
}


static int
storagePoolNumOfVolumes(virStoragePoolPtr pool)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    int ret = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolNumOfVolumesEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    ret = virStoragePoolObjNumOfVolumes(obj, pool->conn,
                                        virStoragePoolNumOfVolumesCheckACL);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}


static int
storagePoolListVolumes(virStoragePoolPtr pool,
                       char **const names,
                       int maxnames)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    int n = -1;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolListVolumesEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    n = virStoragePoolObjVolumeGetNames(obj, pool->conn,
                                        virStoragePoolListVolumesCheckACL,
                                        names, maxnames);
 cleanup:
    virStoragePoolObjUnlock(obj);
    return n;
}


static int
storagePoolListAllVolumes(virStoragePoolPtr pool,
                          virStorageVolPtr **vols,
                          unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    int ret = -1;

    virCheckFlags(0, -1);

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStoragePoolListAllVolumesEnsureACL(pool->conn, def) < 0)
        goto cleanup;

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    ret = virStoragePoolObjVolumeListExport(pool->conn, obj, vols,
                                            virStoragePoolListAllVolumesCheckACL);


 cleanup:
    virStoragePoolObjUnlock(obj);

    return ret;
}

static virStorageVolPtr
storageVolLookupByName(virStoragePoolPtr pool,
                       const char *name)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageVolDefPtr voldef;
    virStorageVolPtr vol = NULL;

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return NULL;
    def = virStoragePoolObjGetDef(obj);

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    voldef = virStorageVolDefFindByName(obj, name);

    if (!voldef) {
        virReportError(VIR_ERR_NO_STORAGE_VOL,
                       _("no storage vol with matching name '%s'"),
                       name);
        goto cleanup;
    }

    if (virStorageVolLookupByNameEnsureACL(pool->conn, def, voldef) < 0)
        goto cleanup;

    vol = virGetStorageVol(pool->conn, def->name, voldef->name,
                           voldef->key, NULL, NULL);

 cleanup:
    virStoragePoolObjUnlock(obj);
    return vol;
}


static virStorageVolPtr
storageVolLookupByKey(virConnectPtr conn,
                      const char *key)
{
    size_t i;
    virStorageVolPtr vol = NULL;

    storageDriverLock();
    for (i = 0; i < driver->pools.count && !vol; i++) {
        virStoragePoolObjPtr obj = driver->pools.objs[i];
        virStoragePoolDefPtr def;

        virStoragePoolObjLock(obj);
        def = virStoragePoolObjGetDef(obj);
        if (virStoragePoolObjIsActive(obj)) {
            virStorageVolDefPtr voldef = virStorageVolDefFindByKey(obj, key);

            if (voldef) {
                if (virStorageVolLookupByKeyEnsureACL(conn, def, voldef) < 0) {
                    virStoragePoolObjUnlock(obj);
                    goto cleanup;
                }

                vol = virGetStorageVol(conn, def->name,
                                       voldef->name, voldef->key,
                                       NULL, NULL);
            }
        }
        virStoragePoolObjUnlock(obj);
    }

    if (!vol)
        virReportError(VIR_ERR_NO_STORAGE_VOL,
                       _("no storage vol with matching key %s"), key);

 cleanup:
    storageDriverUnlock();
    return vol;
}

static virStorageVolPtr
storageVolLookupByPath(virConnectPtr conn,
                       const char *path)
{
    size_t i;
    virStorageVolPtr vol = NULL;
    char *cleanpath;

    cleanpath = virFileSanitizePath(path);
    if (!cleanpath)
        return NULL;

    storageDriverLock();
    for (i = 0; i < driver->pools.count && !vol; i++) {
        virStoragePoolObjPtr obj = driver->pools.objs[i];
        virStoragePoolDefPtr def;
        virStorageVolDefPtr voldef;
        char *stable_path = NULL;

        virStoragePoolObjLock(obj);
        def = virStoragePoolObjGetDef(obj);

        if (!virStoragePoolObjIsActive(obj)) {
           virStoragePoolObjUnlock(obj);
           continue;
        }

        switch ((virStoragePoolType) def->type) {
            case VIR_STORAGE_POOL_DIR:
            case VIR_STORAGE_POOL_FS:
            case VIR_STORAGE_POOL_NETFS:
            case VIR_STORAGE_POOL_LOGICAL:
            case VIR_STORAGE_POOL_DISK:
            case VIR_STORAGE_POOL_ISCSI:
            case VIR_STORAGE_POOL_SCSI:
            case VIR_STORAGE_POOL_MPATH:
            case VIR_STORAGE_POOL_VSTORAGE:
                stable_path = virStorageBackendStablePath(obj,
                                                          cleanpath,
                                                          false);
                if (stable_path == NULL) {
                    /* Don't break the whole lookup process if it fails on
                     * getting the stable path for some of the pools.
                     */
                    VIR_WARN("Failed to get stable path for pool '%s'",
                             def->name);
                    virStoragePoolObjUnlock(obj);
                    continue;
                }
                break;

            case VIR_STORAGE_POOL_GLUSTER:
            case VIR_STORAGE_POOL_RBD:
            case VIR_STORAGE_POOL_SHEEPDOG:
            case VIR_STORAGE_POOL_ZFS:
            case VIR_STORAGE_POOL_LAST:
                if (VIR_STRDUP(stable_path, path) < 0) {
                     virStoragePoolObjUnlock(obj);
                    goto cleanup;
                }
                break;
        }

        voldef = virStorageVolDefFindByPath(obj, stable_path);
        VIR_FREE(stable_path);

        if (voldef) {
            if (virStorageVolLookupByPathEnsureACL(conn, def, voldef) < 0) {
                virStoragePoolObjUnlock(obj);
                goto cleanup;
            }

            vol = virGetStorageVol(conn, def->name,
                                   voldef->name, voldef->key,
                                   NULL, NULL);
        }

        virStoragePoolObjUnlock(obj);
    }

    if (!vol) {
        if (STREQ(path, cleanpath)) {
            virReportError(VIR_ERR_NO_STORAGE_VOL,
                           _("no storage vol with matching path '%s'"), path);
        } else {
            virReportError(VIR_ERR_NO_STORAGE_VOL,
                           _("no storage vol with matching path '%s' (%s)"),
                           path, cleanpath);
        }
    }

 cleanup:
    VIR_FREE(cleanpath);
    storageDriverUnlock();
    return vol;
}

virStoragePoolPtr
storagePoolLookupByTargetPath(virConnectPtr conn,
                              const char *path)
{
    size_t i;
    virStoragePoolPtr pool = NULL;
    char *cleanpath;

    cleanpath = virFileSanitizePath(path);
    if (!cleanpath)
        return NULL;

    storageDriverLock();
    for (i = 0; i < driver->pools.count && !pool; i++) {
        virStoragePoolObjPtr obj = driver->pools.objs[i];
        virStoragePoolDefPtr def;

        virStoragePoolObjLock(obj);
        def = virStoragePoolObjGetDef(obj);

        if (!virStoragePoolObjIsActive(obj)) {
            virStoragePoolObjUnlock(obj);
            continue;
        }

        if (STREQ(path, def->target.path))
            pool = virGetStoragePool(conn, def->name, def->uuid, NULL, NULL);

        virStoragePoolObjUnlock(obj);
    }
    storageDriverUnlock();

    if (!pool) {
        virReportError(VIR_ERR_NO_STORAGE_VOL,
                       _("no storage pool with matching target path '%s'"),
                       path);
    }

    VIR_FREE(cleanpath);
    return pool;
}


static int
storageVolDeleteInternal(virStorageVolPtr vol,
                         virStorageBackendPtr backend,
                         virStoragePoolObjPtr obj,
                         virStorageVolDefPtr voldef,
                         unsigned int flags,
                         bool updateMeta)
{
    virStoragePoolDefPtr def = virStoragePoolObjGetDef(obj);
    int ret = -1;

    if (!backend->deleteVol) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       "%s", _("storage pool does not support vol deletion"));

        goto cleanup;
    }

    if (backend->deleteVol(vol->conn, obj, voldef, flags) < 0)
        goto cleanup;

    /* Update pool metadata - don't update meta data from error paths
     * in this module since the allocation/available weren't adjusted yet.
     * Ignore the disk backend since it updates the pool values.
     */
    if (updateMeta) {
        if (def->type != VIR_STORAGE_POOL_DISK) {
            def->allocation -= voldef->target.allocation;
            def->available += voldef->target.allocation;
        }
    }

    virStoragePoolObjRemoveVol(obj, voldef);
    ret = 0;

 cleanup:
    return ret;
}


static virStorageVolDefPtr
virStorageVolDefFromVol(virStorageVolPtr vol,
                        virStoragePoolObjPtr *obj,
                        virStorageBackendPtr *backend)
{
    virStorageVolDefPtr voldef = NULL;
    virStoragePoolDefPtr def;

    if (!(*obj = storagePoolObjFindByName(vol->pool)))
        return NULL;
    def = virStoragePoolObjGetDef(*obj);

    if (!virStoragePoolObjIsActive(*obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"),
                       def->name);
        goto error;
    }

    if (!(voldef = virStorageVolDefFindByName(*obj, vol->name))) {
        virReportError(VIR_ERR_NO_STORAGE_VOL,
                       _("no storage vol with matching name '%s'"),
                       vol->name);
        goto error;
    }

    if (backend) {
        if (!(*backend = virStorageBackendForType(def->type)))
            goto error;
    }

    return voldef;

 error:
    virStoragePoolObjUnlock(*obj);
    *obj = NULL;

    return NULL;
}


static int
storageVolDelete(virStorageVolPtr vol,
                 unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStorageBackendPtr backend;
    virStorageVolDefPtr voldef = NULL;
    int ret = -1;

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return -1;

    if (virStorageVolDeleteEnsureACL(vol->conn, virStoragePoolObjGetDef(obj),
                                     voldef) < 0)
        goto cleanup;

    if (voldef->in_use) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still in use."),
                       voldef->name);
        goto cleanup;
    }

    if (voldef->building) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still being allocated."),
                       voldef->name);
        goto cleanup;
    }

    if (storageVolDeleteInternal(vol, backend, obj, voldef, flags, true) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}


static virStorageVolPtr
storageVolCreateXML(virStoragePoolPtr pool,
                    const char *xmldesc,
                    unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virStorageVolDefPtr voldef = NULL;
    virStorageVolPtr vol = NULL, newvol = NULL;

    virCheckFlags(VIR_STORAGE_VOL_CREATE_PREALLOC_METADATA, NULL);

    if (!(obj = virStoragePoolObjFromStoragePool(pool)))
        return NULL;
    def = virStoragePoolObjGetDef(obj);

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    voldef = virStorageVolDefParseString(def, xmldesc,
                                         VIR_VOL_XML_PARSE_OPT_CAPACITY);
    if (voldef == NULL)
        goto cleanup;

    if (!voldef->target.capacity && !backend->buildVol) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       "%s", _("volume capacity required for this "
                               "storage pool"));
        goto cleanup;
    }

    if (virStorageVolCreateXMLEnsureACL(pool->conn, def, voldef) < 0)
        goto cleanup;

    if (virStorageVolDefFindByName(obj, voldef->name)) {
        virReportError(VIR_ERR_STORAGE_VOL_EXIST,
                       _("'%s'"), voldef->name);
        goto cleanup;
    }

    if (!backend->createVol) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       "%s", _("storage pool does not support volume "
                               "creation"));
        goto cleanup;
    }

    /* Wipe any key the user may have suggested, as volume creation
     * will generate the canonical key.  */
    VIR_FREE(voldef->key);
    if (backend->createVol(pool->conn, obj, voldef) < 0)
        goto cleanup;

    if (!(newvol = virGetStorageVol(pool->conn, def->name, voldef->name,
                                    voldef->key, NULL, NULL)))
        goto cleanup;

    /* NB: Upon success voldef "owned" by storage pool for deletion purposes */
    if (virStoragePoolObjAddVol(obj, voldef) < 0)
        goto cleanup;

    if (backend->buildVol) {
        int buildret;
        virStorageVolDefPtr buildvoldef = NULL;

        if (VIR_ALLOC(buildvoldef) < 0) {
            voldef = NULL;
            goto cleanup;
        }

        /* Make a shallow copy of the 'defined' volume definition, since the
         * original allocation value will change as the user polls 'info',
         * but we only need the initial requested values
         */
        memcpy(buildvoldef, voldef, sizeof(*voldef));

        /* Drop the pool lock during volume allocation */
        virStoragePoolObjIncrAsyncjobs(obj);
        voldef->building = true;
        virStoragePoolObjUnlock(obj);

        buildret = backend->buildVol(pool->conn, obj, buildvoldef, flags);

        VIR_FREE(buildvoldef);

        storageDriverLock();
        virStoragePoolObjLock(obj);
        storageDriverUnlock();

        voldef->building = false;
        virStoragePoolObjDecrAsyncjobs(obj);

        if (buildret < 0) {
            /* buildVol handles deleting volume on failure */
            virStoragePoolObjRemoveVol(obj, voldef);
            voldef = NULL;
            goto cleanup;
        }

    }

    if (backend->refreshVol &&
        backend->refreshVol(pool->conn, obj, voldef) < 0) {
        storageVolDeleteInternal(newvol, backend, obj, voldef,
                                 0, false);
        voldef = NULL;
        goto cleanup;
    }

    /* Update pool metadata ignoring the disk backend since
     * it updates the pool values.
     */
    if (def->type != VIR_STORAGE_POOL_DISK) {
        def->allocation += voldef->target.allocation;
        def->available -= voldef->target.allocation;
    }

    VIR_INFO("Creating volume '%s' in storage pool '%s'",
             newvol->name, def->name);
    vol = newvol;
    newvol = NULL;
    voldef = NULL;

 cleanup:
    virObjectUnref(newvol);
    virStorageVolDefFree(voldef);
    if (obj)
        virStoragePoolObjUnlock(obj);
    return vol;
}

static virStorageVolPtr
storageVolCreateXMLFrom(virStoragePoolPtr pool,
                        const char *xmldesc,
                        virStorageVolPtr volsrc,
                        unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStoragePoolObjPtr objsrc = NULL;
    virStorageBackendPtr backend;
    virStorageVolDefPtr voldefsrc = NULL;
    virStorageVolDefPtr voldef = NULL;
    virStorageVolDefPtr shadowvol = NULL;
    virStorageVolPtr newvol = NULL;
    virStorageVolPtr vol = NULL;
    int buildret;

    virCheckFlags(VIR_STORAGE_VOL_CREATE_PREALLOC_METADATA |
                  VIR_STORAGE_VOL_CREATE_REFLINK,
                  NULL);

    storageDriverLock();
    obj = virStoragePoolObjFindByUUID(&driver->pools, pool->uuid);
    if (obj && STRNEQ(pool->name, volsrc->pool)) {
        virStoragePoolObjUnlock(obj);
        objsrc = virStoragePoolObjFindByName(&driver->pools, volsrc->pool);
        virStoragePoolObjLock(obj);
    }
    storageDriverUnlock();
    if (!obj) {
        char uuidstr[VIR_UUID_STRING_BUFLEN];
        virUUIDFormat(pool->uuid, uuidstr);
        virReportError(VIR_ERR_NO_STORAGE_POOL,
                       _("no storage pool with matching uuid '%s' (%s)"),
                       uuidstr, pool->name);
        goto cleanup;
    }
    def = virStoragePoolObjGetDef(obj);

    if (STRNEQ(pool->name, volsrc->pool) && !objsrc) {
        virReportError(VIR_ERR_NO_STORAGE_POOL,
                       _("no storage pool with matching name '%s'"),
                       volsrc->pool);
        goto cleanup;
    }

    if (!virStoragePoolObjIsActive(obj)) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"), def->name);
        goto cleanup;
    }

    if (objsrc && !virStoragePoolObjIsActive(objsrc)) {
        virStoragePoolDefPtr objsrcdef = virStoragePoolObjGetDef(objsrc);
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("storage pool '%s' is not active"),
                       objsrcdef->name);
        goto cleanup;
    }

    if ((backend = virStorageBackendForType(def->type)) == NULL)
        goto cleanup;

    voldefsrc = virStorageVolDefFindByName(objsrc ?
                                           objsrc : obj, volsrc->name);
    if (!voldefsrc) {
        virReportError(VIR_ERR_NO_STORAGE_VOL,
                       _("no storage vol with matching name '%s'"),
                       volsrc->name);
        goto cleanup;
    }

    voldef = virStorageVolDefParseString(def, xmldesc,
                                         VIR_VOL_XML_PARSE_NO_CAPACITY);
    if (voldef == NULL)
        goto cleanup;

    if (virStorageVolCreateXMLFromEnsureACL(pool->conn, def, voldef) < 0)
        goto cleanup;

    if (virStorageVolDefFindByName(obj, voldef->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("storage volume name '%s' already in use."),
                       voldef->name);
        goto cleanup;
    }

    /* Use the original volume's capacity in case the new capacity
     * is less than that, or it was omitted */
    if (voldef->target.capacity < voldefsrc->target.capacity)
        voldef->target.capacity = voldefsrc->target.capacity;

    /* If the allocation was not provided in the XML, then use capacity
     * as it's specifically documented "If omitted when creating a volume,
     * the  volume will be fully allocated at time of creation.". This
     * is especially important for logical volume creation. */
    if (!voldef->target.has_allocation)
        voldef->target.allocation = voldef->target.capacity;

    if (!backend->buildVolFrom) {
        virReportError(VIR_ERR_NO_SUPPORT,
                       "%s", _("storage pool does not support"
                               " volume creation from an existing volume"));
        goto cleanup;
    }

    if (voldefsrc->building) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still being allocated."),
                       voldefsrc->name);
        goto cleanup;
    }

    if (backend->refreshVol &&
        backend->refreshVol(pool->conn, obj, voldefsrc) < 0)
        goto cleanup;

    /* 'Define' the new volume so we get async progress reporting.
     * Wipe any key the user may have suggested, as volume creation
     * will generate the canonical key.  */
    VIR_FREE(voldef->key);
    if (backend->createVol(pool->conn, obj, voldef) < 0)
        goto cleanup;

    /* Make a shallow copy of the 'defined' volume definition, since the
     * original allocation value will change as the user polls 'info',
     * but we only need the initial requested values
     */
    if (VIR_ALLOC(shadowvol) < 0)
        goto cleanup;

    memcpy(shadowvol, voldef, sizeof(*voldef));

    if (!(newvol = virGetStorageVol(pool->conn, def->name, voldef->name,
                                    voldef->key, NULL, NULL)))
        goto cleanup;

    /* NB: Upon success voldef "owned" by storage pool for deletion purposes */
    if (virStoragePoolObjAddVol(obj, voldef) < 0)
        goto cleanup;

    /* Drop the pool lock during volume allocation */
    virStoragePoolObjIncrAsyncjobs(obj);
    voldef->building = true;
    voldefsrc->in_use++;
    virStoragePoolObjUnlock(obj);

    if (objsrc) {
        virStoragePoolObjIncrAsyncjobs(objsrc);
        virStoragePoolObjUnlock(objsrc);
    }

    buildret = backend->buildVolFrom(pool->conn, obj, shadowvol, voldefsrc, flags);

    storageDriverLock();
    virStoragePoolObjLock(obj);
    if (objsrc)
        virStoragePoolObjLock(objsrc);
    storageDriverUnlock();

    voldefsrc->in_use--;
    voldef->building = false;
    virStoragePoolObjDecrAsyncjobs(obj);

    if (objsrc) {
        virStoragePoolObjDecrAsyncjobs(objsrc);
        virStoragePoolObjUnlock(objsrc);
        objsrc = NULL;
    }

    if (buildret < 0 ||
        (backend->refreshVol &&
         backend->refreshVol(pool->conn, obj, voldef) < 0)) {
        storageVolDeleteInternal(newvol, backend, obj, voldef, 0, false);
        voldef = NULL;
        goto cleanup;
    }

    /* Updating pool metadata ignoring the disk backend since
     * it updates the pool values
     */
    if (def->type != VIR_STORAGE_POOL_DISK) {
        def->allocation += voldef->target.allocation;
        def->available -= voldef->target.allocation;
    }

    VIR_INFO("Creating volume '%s' in storage pool '%s'",
             newvol->name, def->name);
    vol = newvol;
    newvol = NULL;
    voldef = NULL;

 cleanup:
    virObjectUnref(newvol);
    virStorageVolDefFree(voldef);
    VIR_FREE(shadowvol);
    if (obj)
        virStoragePoolObjUnlock(obj);
    if (objsrc)
        virStoragePoolObjUnlock(objsrc);
    return vol;
}


static int
storageVolDownload(virStorageVolPtr vol,
                   virStreamPtr stream,
                   unsigned long long offset,
                   unsigned long long length,
                   unsigned int flags)
{
    virStorageBackendPtr backend;
    virStoragePoolObjPtr obj = NULL;
    virStorageVolDefPtr voldef = NULL;
    int ret = -1;

    virCheckFlags(VIR_STORAGE_VOL_DOWNLOAD_SPARSE_STREAM, -1);

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return -1;

    if (virStorageVolDownloadEnsureACL(vol->conn, virStoragePoolObjGetDef(obj),
                                       voldef) < 0)
        goto cleanup;

    if (voldef->building) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still being allocated."),
                       voldef->name);
        goto cleanup;
    }

    if (!backend->downloadVol) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("storage pool doesn't support volume download"));
        goto cleanup;
    }

    ret = backend->downloadVol(vol->conn, obj, voldef, stream,
                               offset, length, flags);

 cleanup:
    virStoragePoolObjUnlock(obj);

    return ret;
}


/**
 * Frees opaque data.
 *
 * @opaque Data to be freed.
 */
static void
virStorageVolPoolRefreshDataFree(void *opaque)
{
    virStorageVolStreamInfoPtr cbdata = opaque;

    VIR_FREE(cbdata->pool_name);
    VIR_FREE(cbdata);
}

static int
virStorageBackendPloopRestoreDesc(char *path)
{
    int ret = -1;
    virCommandPtr cmd = NULL;
    char *refresh_tool = NULL;
    char *desc = NULL;

    if (virAsprintf(&desc, "%s/DiskDescriptor.xml", path) < 0)
        return ret;

    if (virFileRemove(desc, 0, 0) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("refresh ploop failed:"
                         " unable to delete DiskDescriptor.xml"));
        goto cleanup;
    }

    refresh_tool = virFindFileInPath("ploop");
    if (!refresh_tool) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unable to find ploop, please install ploop tools"));
        goto cleanup;
    }

    cmd = virCommandNewArgList(refresh_tool, "restore-descriptor",
                               path, NULL);
    virCommandAddArgFormat(cmd, "%s/root.hds", path);
    if (virCommandRun(cmd, NULL) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    VIR_FREE(refresh_tool);
    virCommandFree(cmd);
    VIR_FREE(desc);
    return ret;
}



/**
 * Thread to handle the pool refresh
 *
 * @st Pointer to stream being closed.
 * @opaque Domain's device information structure.
 */
static void
virStorageVolPoolRefreshThread(void *opaque)
{

    virStorageVolStreamInfoPtr cbdata = opaque;
    virStoragePoolObjPtr obj = NULL;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virObjectEventPtr event = NULL;

    storageDriverLock();
    if (cbdata->vol_path) {
        if (virStorageBackendPloopRestoreDesc(cbdata->vol_path) < 0)
            goto cleanup;
    }
    if (!(obj = virStoragePoolObjFindByName(&driver->pools,
                                            cbdata->pool_name)))
        goto cleanup;
    def = virStoragePoolObjGetDef(obj);

    /* If some thread is building a new volume in the pool, then we cannot
     * clear out all vols and refresh the pool. So we'll just pass. */
    if (virStoragePoolObjGetAsyncjobs(obj) > 0) {
        VIR_DEBUG("Asyncjob in process, cannot refresh storage pool");
        goto cleanup;
    }

    if (!(backend = virStorageBackendForType(def->type)))
        goto cleanup;

    virStoragePoolObjClearVols(obj);
    if (backend->refreshPool(NULL, obj) < 0)
        VIR_DEBUG("Failed to refresh storage pool");

    event = virStoragePoolEventRefreshNew(def->name, def->uuid);

 cleanup:
    if (event)
        virObjectEventStateQueue(driver->storageEventState, event);
    if (obj)
        virStoragePoolObjUnlock(obj);
    storageDriverUnlock();
    virStorageVolPoolRefreshDataFree(cbdata);
}

/**
 * Callback being called if a FDstream is closed. Will spin off a thread
 * to perform a pool refresh.
 *
 * @st Pointer to stream being closed.
 * @opaque Buffer to hold the pool name to be refreshed
 */
static void
virStorageVolFDStreamCloseCb(virStreamPtr st ATTRIBUTE_UNUSED,
                             void *opaque)
{
    virThread thread;

    if (virThreadCreate(&thread, false, virStorageVolPoolRefreshThread,
                        opaque) < 0) {
        /* Not much else can be done */
        VIR_ERROR(_("Failed to create thread to handle pool refresh"));
        goto error;
    }
    return; /* Thread will free opaque data */

 error:
    virStorageVolPoolRefreshDataFree(opaque);
}

static int
storageVolUpload(virStorageVolPtr vol,
                 virStreamPtr stream,
                 unsigned long long offset,
                 unsigned long long length,
                 unsigned int flags)
{
    virStorageBackendPtr backend;
    virStoragePoolObjPtr obj = NULL;
    virStoragePoolDefPtr def;
    virStorageVolDefPtr voldef = NULL;
    virStorageVolStreamInfoPtr cbdata = NULL;
    int ret = -1;

    virCheckFlags(VIR_STORAGE_VOL_UPLOAD_SPARSE_STREAM, -1);

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStorageVolUploadEnsureACL(vol->conn, def, voldef) < 0)
        goto cleanup;

    if (voldef->in_use) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still in use."),
                       voldef->name);
        goto cleanup;
    }

    if (voldef->building) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still being allocated."),
                       voldef->name);
        goto cleanup;
    }

    if (!backend->uploadVol) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("storage pool doesn't support volume upload"));
        goto cleanup;
    }

    /* Use the callback routine in order to
     * refresh the pool after the volume upload stream closes. This way
     * we make sure the volume and pool data are refreshed without user
     * interaction and we can just lookup the backend in the callback
     * routine in order to call the refresh API.
     */
    if (VIR_ALLOC(cbdata) < 0 ||
        VIR_STRDUP(cbdata->pool_name, def->name) < 0)
        goto cleanup;
    if (voldef->type == VIR_STORAGE_VOL_PLOOP &&
        VIR_STRDUP(cbdata->vol_path, voldef->target.path) < 0)
        goto cleanup;

    if ((ret = backend->uploadVol(vol->conn, obj, voldef, stream,
                                  offset, length, flags)) < 0)
        goto cleanup;

    /* Add cleanup callback - call after uploadVol since the stream
     * is then fully set up
     */
    virFDStreamSetInternalCloseCb(stream,
                                  virStorageVolFDStreamCloseCb,
                                  cbdata, NULL);
    cbdata = NULL;

 cleanup:
    virStoragePoolObjUnlock(obj);
    if (cbdata)
        virStorageVolPoolRefreshDataFree(cbdata);

    return ret;
}

static int
storageVolResize(virStorageVolPtr vol,
                 unsigned long long capacity,
                 unsigned int flags)
{
    virStorageBackendPtr backend;
    virStoragePoolObjPtr obj = NULL;
    virStoragePoolDefPtr def;
    virStorageVolDefPtr voldef = NULL;
    unsigned long long abs_capacity, delta = 0;
    int ret = -1;

    virCheckFlags(VIR_STORAGE_VOL_RESIZE_ALLOCATE |
                  VIR_STORAGE_VOL_RESIZE_DELTA |
                  VIR_STORAGE_VOL_RESIZE_SHRINK, -1);

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return -1;
    def = virStoragePoolObjGetDef(obj);

    if (virStorageVolResizeEnsureACL(vol->conn, def, voldef) < 0)
        goto cleanup;

    if (voldef->in_use) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still in use."),
                       voldef->name);
        goto cleanup;
    }

    if (voldef->building) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still being allocated."),
                       voldef->name);
        goto cleanup;
    }

    if (flags & VIR_STORAGE_VOL_RESIZE_DELTA) {
        if (flags & VIR_STORAGE_VOL_RESIZE_SHRINK)
            abs_capacity = voldef->target.capacity - MIN(capacity, voldef->target.capacity);
        else
            abs_capacity = voldef->target.capacity + capacity;
        flags &= ~VIR_STORAGE_VOL_RESIZE_DELTA;
    } else {
        abs_capacity = capacity;
    }

    if (abs_capacity < voldef->target.allocation) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("can't shrink capacity below "
                         "existing allocation"));
        goto cleanup;
    }

    if (abs_capacity < voldef->target.capacity &&
        !(flags & VIR_STORAGE_VOL_RESIZE_SHRINK)) {
        virReportError(VIR_ERR_INVALID_ARG, "%s",
                       _("Can't shrink capacity below current "
                         "capacity unless shrink flag explicitly specified"));
        goto cleanup;
    }

    if (flags & VIR_STORAGE_VOL_RESIZE_ALLOCATE)
        delta = abs_capacity - voldef->target.allocation;

    if (delta > def->available) {
        virReportError(VIR_ERR_OPERATION_FAILED, "%s",
                       _("Not enough space left in storage pool"));
        goto cleanup;
    }

    if (!backend->resizeVol) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("storage pool does not support changing of "
                         "volume capacity"));
        goto cleanup;
    }

    if (backend->resizeVol(vol->conn, obj, voldef, abs_capacity, flags) < 0)
        goto cleanup;

    voldef->target.capacity = abs_capacity;
    /* Only update the allocation and pool values if we actually did the
     * allocation; otherwise, this is akin to a create operation with a
     * capacity value different and potentially much larger than available
     */
    if (flags & VIR_STORAGE_VOL_RESIZE_ALLOCATE) {
        voldef->target.allocation = abs_capacity;
        def->allocation += delta;
        def->available -= delta;
    }

    ret = 0;

 cleanup:
    virStoragePoolObjUnlock(obj);

    return ret;
}


static int
storageVolWipePattern(virStorageVolPtr vol,
                      unsigned int algorithm,
                      unsigned int flags)
{
    virStorageBackendPtr backend;
    virStoragePoolObjPtr obj = NULL;
    virStorageVolDefPtr voldef = NULL;
    int ret = -1;

    virCheckFlags(0, -1);

    if (algorithm >= VIR_STORAGE_VOL_WIPE_ALG_LAST) {
        virReportError(VIR_ERR_INVALID_ARG,
                       _("wiping algorithm %d not supported"),
                       algorithm);
        return -1;
    }

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return -1;

    if (virStorageVolWipePatternEnsureACL(vol->conn,
                                          virStoragePoolObjGetDef(obj),
                                          voldef) < 0)
        goto cleanup;

    if (voldef->in_use) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still in use."),
                       voldef->name);
        goto cleanup;
    }

    if (voldef->building) {
        virReportError(VIR_ERR_OPERATION_INVALID,
                       _("volume '%s' is still being allocated."),
                       voldef->name);
        goto cleanup;
    }

    if (!backend->wipeVol) {
        virReportError(VIR_ERR_NO_SUPPORT, "%s",
                       _("storage pool doesn't support volume wiping"));
        goto cleanup;
    }

    if (backend->wipeVol(vol->conn, obj, voldef, algorithm, flags) < 0)
        goto cleanup;

    /* Instead of using the refreshVol, since much changes on the target
     * volume, let's update using the same function as refreshPool would
     * use when it discovers a volume. The only failure to capture is -1,
     * we can ignore -2. */
    if (virStorageBackendRefreshVolTargetUpdate(voldef) == -1)
        goto cleanup;

    ret = 0;

 cleanup:
    virStoragePoolObjUnlock(obj);

    return ret;
}

static int
storageVolWipe(virStorageVolPtr vol,
               unsigned int flags)
{
    return storageVolWipePattern(vol, VIR_STORAGE_VOL_WIPE_ALG_ZERO, flags);
}


static int
storageVolGetInfoFlags(virStorageVolPtr vol,
                       virStorageVolInfoPtr info,
                       unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStorageBackendPtr backend;
    virStorageVolDefPtr voldef;
    int ret = -1;

    virCheckFlags(VIR_STORAGE_VOL_GET_PHYSICAL, -1);

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return -1;

    if (virStorageVolGetInfoFlagsEnsureACL(vol->conn,
                                           virStoragePoolObjGetDef(obj),
                                           voldef) < 0)
        goto cleanup;

    if (backend->refreshVol &&
        backend->refreshVol(vol->conn, obj, voldef) < 0)
        goto cleanup;

    memset(info, 0, sizeof(*info));
    info->type = voldef->type;
    info->capacity = voldef->target.capacity;
    if (flags & VIR_STORAGE_VOL_GET_PHYSICAL)
        info->allocation = voldef->target.physical;
    else
        info->allocation = voldef->target.allocation;
    ret = 0;

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}


static int
storageVolGetInfo(virStorageVolPtr vol,
                  virStorageVolInfoPtr info)
{
    return storageVolGetInfoFlags(vol, info, 0);
}


static char *
storageVolGetXMLDesc(virStorageVolPtr vol,
                     unsigned int flags)
{
    virStoragePoolObjPtr obj;
    virStoragePoolDefPtr def;
    virStorageBackendPtr backend;
    virStorageVolDefPtr voldef;
    char *ret = NULL;

    virCheckFlags(0, NULL);

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, &backend)))
        return NULL;
    def = virStoragePoolObjGetDef(obj);

    if (virStorageVolGetXMLDescEnsureACL(vol->conn, def, voldef) < 0)
        goto cleanup;

    if (backend->refreshVol &&
        backend->refreshVol(vol->conn, obj, voldef) < 0)
        goto cleanup;

    ret = virStorageVolDefFormat(def, voldef);

 cleanup:
    virStoragePoolObjUnlock(obj);

    return ret;
}

static char *
storageVolGetPath(virStorageVolPtr vol)
{
    virStoragePoolObjPtr obj;
    virStorageVolDefPtr voldef;
    char *ret = NULL;

    if (!(voldef = virStorageVolDefFromVol(vol, &obj, NULL)))
        return NULL;

    if (virStorageVolGetPathEnsureACL(vol->conn, virStoragePoolObjGetDef(obj),
                                      voldef) < 0)
        goto cleanup;

    ignore_value(VIR_STRDUP(ret, voldef->target.path));

 cleanup:
    virStoragePoolObjUnlock(obj);
    return ret;
}

static int
storageConnectListAllStoragePools(virConnectPtr conn,
                                  virStoragePoolPtr **pools,
                                  unsigned int flags)
{
    int ret = -1;

    virCheckFlags(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_ALL, -1);

    if (virConnectListAllStoragePoolsEnsureACL(conn) < 0)
        goto cleanup;

    storageDriverLock();
    ret = virStoragePoolObjListExport(conn, &driver->pools, pools,
                                      virConnectListAllStoragePoolsCheckACL,
                                      flags);
    storageDriverUnlock();

 cleanup:
    return ret;
}

static int
storageConnectStoragePoolEventRegisterAny(virConnectPtr conn,
                                          virStoragePoolPtr pool,
                                          int eventID,
                                          virConnectStoragePoolEventGenericCallback callback,
                                          void *opaque,
                                          virFreeCallback freecb)
{
    int callbackID = -1;

    if (virConnectStoragePoolEventRegisterAnyEnsureACL(conn) < 0)
        goto cleanup;

    if (virStoragePoolEventStateRegisterID(conn, driver->storageEventState,
                                           pool, eventID, callback,
                                           opaque, freecb, &callbackID) < 0)
        callbackID = -1;
 cleanup:
    return callbackID;
}

static int
storageConnectStoragePoolEventDeregisterAny(virConnectPtr conn,
                                            int callbackID)
{
    int ret = -1;

    if (virConnectStoragePoolEventDeregisterAnyEnsureACL(conn) < 0)
        goto cleanup;

    if (virObjectEventStateDeregisterID(conn,
                                        driver->storageEventState,
                                        callbackID, true) < 0)
        goto cleanup;

    ret = 0;

 cleanup:
    return ret;
}



static virStorageDriver storageDriver = {
    .name = "storage",
    .connectNumOfStoragePools = storageConnectNumOfStoragePools, /* 0.4.0 */
    .connectListStoragePools = storageConnectListStoragePools, /* 0.4.0 */
    .connectNumOfDefinedStoragePools = storageConnectNumOfDefinedStoragePools, /* 0.4.0 */
    .connectListDefinedStoragePools = storageConnectListDefinedStoragePools, /* 0.4.0 */
    .connectListAllStoragePools = storageConnectListAllStoragePools, /* 0.10.2 */
    .connectStoragePoolEventRegisterAny = storageConnectStoragePoolEventRegisterAny, /* 2.0.0 */
    .connectStoragePoolEventDeregisterAny = storageConnectStoragePoolEventDeregisterAny, /* 2.0.0 */
    .connectFindStoragePoolSources = storageConnectFindStoragePoolSources, /* 0.4.0 */
    .storagePoolLookupByName = storagePoolLookupByName, /* 0.4.0 */
    .storagePoolLookupByUUID = storagePoolLookupByUUID, /* 0.4.0 */
    .storagePoolLookupByVolume = storagePoolLookupByVolume, /* 0.4.0 */
    .storagePoolCreateXML = storagePoolCreateXML, /* 0.4.0 */
    .storagePoolDefineXML = storagePoolDefineXML, /* 0.4.0 */
    .storagePoolBuild = storagePoolBuild, /* 0.4.0 */
    .storagePoolUndefine = storagePoolUndefine, /* 0.4.0 */
    .storagePoolCreate = storagePoolCreate, /* 0.4.0 */
    .storagePoolDestroy = storagePoolDestroy, /* 0.4.0 */
    .storagePoolDelete = storagePoolDelete, /* 0.4.0 */
    .storagePoolRefresh = storagePoolRefresh, /* 0.4.0 */
    .storagePoolGetInfo = storagePoolGetInfo, /* 0.4.0 */
    .storagePoolGetXMLDesc = storagePoolGetXMLDesc, /* 0.4.0 */
    .storagePoolGetAutostart = storagePoolGetAutostart, /* 0.4.0 */
    .storagePoolSetAutostart = storagePoolSetAutostart, /* 0.4.0 */
    .storagePoolNumOfVolumes = storagePoolNumOfVolumes, /* 0.4.0 */
    .storagePoolListVolumes = storagePoolListVolumes, /* 0.4.0 */
    .storagePoolListAllVolumes = storagePoolListAllVolumes, /* 0.10.2 */

    .storageVolLookupByName = storageVolLookupByName, /* 0.4.0 */
    .storageVolLookupByKey = storageVolLookupByKey, /* 0.4.0 */
    .storageVolLookupByPath = storageVolLookupByPath, /* 0.4.0 */
    .storageVolCreateXML = storageVolCreateXML, /* 0.4.0 */
    .storageVolCreateXMLFrom = storageVolCreateXMLFrom, /* 0.6.4 */
    .storageVolDownload = storageVolDownload, /* 0.9.0 */
    .storageVolUpload = storageVolUpload, /* 0.9.0 */
    .storageVolDelete = storageVolDelete, /* 0.4.0 */
    .storageVolWipe = storageVolWipe, /* 0.8.0 */
    .storageVolWipePattern = storageVolWipePattern, /* 0.9.10 */
    .storageVolGetInfo = storageVolGetInfo, /* 0.4.0 */
    .storageVolGetInfoFlags = storageVolGetInfoFlags, /* 3.0.0 */
    .storageVolGetXMLDesc = storageVolGetXMLDesc, /* 0.4.0 */
    .storageVolGetPath = storageVolGetPath, /* 0.4.0 */
    .storageVolResize = storageVolResize, /* 0.9.10 */

    .storagePoolIsActive = storagePoolIsActive, /* 0.7.3 */
    .storagePoolIsPersistent = storagePoolIsPersistent, /* 0.7.3 */
};


static virStateDriver stateDriver = {
    .name = "storage",
    .stateInitialize = storageStateInitialize,
    .stateAutoStart = storageStateAutoStart,
    .stateCleanup = storageStateCleanup,
    .stateReload = storageStateReload,
};

static int
storageRegisterFull(bool allbackends)
{
    if (virStorageBackendDriversRegister(allbackends) < 0)
        return -1;
    if (virSetSharedStorageDriver(&storageDriver) < 0)
        return -1;
    if (virRegisterStateDriver(&stateDriver) < 0)
        return -1;
    return 0;
}


int
storageRegister(void)
{
    return storageRegisterFull(false);
}


int
storageRegisterAll(void)
{
    return storageRegisterFull(true);
}


static int
virStorageAddISCSIPoolSourceHost(virDomainDiskDefPtr def,
                                 virStoragePoolDefPtr pooldef)
{
    int ret = -1;
    char **tokens = NULL;

    /* Only support one host */
    if (pooldef->source.nhost != 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Expected exactly 1 host for the storage pool"));
        goto cleanup;
    }

    /* iscsi pool only supports one host */
    def->src->nhosts = 1;

    if (VIR_ALLOC_N(def->src->hosts, def->src->nhosts) < 0)
        goto cleanup;

    if (VIR_STRDUP(def->src->hosts[0].name, pooldef->source.hosts[0].name) < 0)
        goto cleanup;

    def->src->hosts[0].port = pooldef->source.hosts[0].port ?
        pooldef->source.hosts[0].port : 3260;

    /* iscsi volume has name like "unit:0:0:1" */
    if (!(tokens = virStringSplit(def->src->srcpool->volume, ":", 0)))
        goto cleanup;

    if (virStringListLength((const char * const *)tokens) != 4) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unexpected iscsi volume name '%s'"),
                       def->src->srcpool->volume);
        goto cleanup;
    }

    /* iscsi pool has only one source device path */
    if (virAsprintf(&def->src->path, "%s/%s",
                    pooldef->source.devices[0].path,
                    tokens[3]) < 0)
        goto cleanup;

    /* Storage pool have not supported these 2 attributes yet,
     * use the defaults.
     */
    def->src->hosts[0].transport = VIR_STORAGE_NET_HOST_TRANS_TCP;
    def->src->hosts[0].socket = NULL;

    def->src->protocol = VIR_STORAGE_NET_PROTOCOL_ISCSI;

    ret = 0;

 cleanup:
    virStringListFree(tokens);
    return ret;
}


static int
virStorageTranslateDiskSourcePoolAuth(virDomainDiskDefPtr def,
                                      virStoragePoolSourcePtr source)
{
    int ret = -1;

    /* Only necessary when authentication set */
    if (!source->auth) {
        ret = 0;
        goto cleanup;
    }
    def->src->auth = virStorageAuthDefCopy(source->auth);
    if (!def->src->auth)
        goto cleanup;
    /* A <disk> doesn't use <auth type='%s', so clear that out for the disk */
    def->src->auth->authType = VIR_STORAGE_AUTH_TYPE_NONE;
    ret = 0;

 cleanup:
    return ret;
}


int
virStorageTranslateDiskSourcePool(virConnectPtr conn,
                                  virDomainDiskDefPtr def)
{
    virStoragePoolDefPtr pooldef = NULL;
    virStoragePoolPtr pool = NULL;
    virStorageVolPtr vol = NULL;
    char *poolxml = NULL;
    virStorageVolInfo info;
    int ret = -1;

    if (def->src->type != VIR_STORAGE_TYPE_VOLUME)
        return 0;

    if (!def->src->srcpool)
        return 0;

    if (!(pool = virStoragePoolLookupByName(conn, def->src->srcpool->pool)))
        return -1;

    if (virStoragePoolIsActive(pool) != 1) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("storage pool '%s' containing volume '%s' "
                         "is not active"),
                       def->src->srcpool->pool, def->src->srcpool->volume);
        goto cleanup;
    }

    if (!(vol = virStorageVolLookupByName(pool, def->src->srcpool->volume)))
        goto cleanup;

    if (virStorageVolGetInfo(vol, &info) < 0)
        goto cleanup;

    if (!(poolxml = virStoragePoolGetXMLDesc(pool, 0)))
        goto cleanup;

    if (!(pooldef = virStoragePoolDefParseString(poolxml)))
        goto cleanup;

    def->src->srcpool->pooltype = pooldef->type;
    def->src->srcpool->voltype = info.type;

    if (def->src->srcpool->mode && pooldef->type != VIR_STORAGE_POOL_ISCSI) {
        virReportError(VIR_ERR_XML_ERROR, "%s",
                       _("disk source mode is only valid when "
                         "storage pool is of iscsi type"));
        goto cleanup;
    }

    VIR_FREE(def->src->path);
    virStorageNetHostDefFree(def->src->nhosts, def->src->hosts);
    def->src->nhosts = 0;
    def->src->hosts = NULL;
    virStorageAuthDefFree(def->src->auth);
    def->src->auth = NULL;

    switch ((virStoragePoolType) pooldef->type) {
    case VIR_STORAGE_POOL_DIR:
    case VIR_STORAGE_POOL_FS:
    case VIR_STORAGE_POOL_NETFS:
    case VIR_STORAGE_POOL_LOGICAL:
    case VIR_STORAGE_POOL_DISK:
    case VIR_STORAGE_POOL_SCSI:
    case VIR_STORAGE_POOL_ZFS:
    case VIR_STORAGE_POOL_VSTORAGE:
        if (!(def->src->path = virStorageVolGetPath(vol)))
            goto cleanup;

        if (def->startupPolicy && info.type != VIR_STORAGE_VOL_FILE) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("'startupPolicy' is only valid for "
                             "'file' type volume"));
            goto cleanup;
        }


        switch (info.type) {
        case VIR_STORAGE_VOL_FILE:
            def->src->srcpool->actualtype = VIR_STORAGE_TYPE_FILE;
            break;

        case VIR_STORAGE_VOL_DIR:
            def->src->srcpool->actualtype = VIR_STORAGE_TYPE_DIR;
            break;

        case VIR_STORAGE_VOL_BLOCK:
            def->src->srcpool->actualtype = VIR_STORAGE_TYPE_BLOCK;
            break;

        case VIR_STORAGE_VOL_PLOOP:
            def->src->srcpool->actualtype = VIR_STORAGE_TYPE_FILE;
            break;

        case VIR_STORAGE_VOL_NETWORK:
        case VIR_STORAGE_VOL_NETDIR:
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("unexpected storage volume type '%s' "
                             "for storage pool type '%s'"),
                           virStorageVolTypeToString(info.type),
                           virStoragePoolTypeToString(pooldef->type));
            goto cleanup;
        }

        break;

    case VIR_STORAGE_POOL_ISCSI:
        if (def->startupPolicy) {
            virReportError(VIR_ERR_XML_ERROR, "%s",
                           _("'startupPolicy' is only valid for "
                             "'file' type volume"));
            goto cleanup;
        }

       switch (def->src->srcpool->mode) {
       case VIR_STORAGE_SOURCE_POOL_MODE_DEFAULT:
       case VIR_STORAGE_SOURCE_POOL_MODE_LAST:
           def->src->srcpool->mode = VIR_STORAGE_SOURCE_POOL_MODE_HOST;
           ATTRIBUTE_FALLTHROUGH;
       case VIR_STORAGE_SOURCE_POOL_MODE_HOST:
           def->src->srcpool->actualtype = VIR_STORAGE_TYPE_BLOCK;
           if (!(def->src->path = virStorageVolGetPath(vol)))
               goto cleanup;
           break;

       case VIR_STORAGE_SOURCE_POOL_MODE_DIRECT:
           def->src->srcpool->actualtype = VIR_STORAGE_TYPE_NETWORK;
           def->src->protocol = VIR_STORAGE_NET_PROTOCOL_ISCSI;

           if (virStorageTranslateDiskSourcePoolAuth(def,
                                                     &pooldef->source) < 0)
               goto cleanup;

           /* Source pool may not fill in the secrettype field,
            * so we need to do so here
            */
           if (def->src->auth && !def->src->auth->secrettype) {
               const char *secrettype =
                   virSecretUsageTypeToString(VIR_SECRET_USAGE_TYPE_ISCSI);
               if (VIR_STRDUP(def->src->auth->secrettype, secrettype) < 0)
                   goto cleanup;
           }

           if (virStorageAddISCSIPoolSourceHost(def, pooldef) < 0)
               goto cleanup;
           break;
       }
       break;

    case VIR_STORAGE_POOL_MPATH:
    case VIR_STORAGE_POOL_RBD:
    case VIR_STORAGE_POOL_SHEEPDOG:
    case VIR_STORAGE_POOL_GLUSTER:
    case VIR_STORAGE_POOL_LAST:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("using '%s' pools for backing 'volume' disks "
                         "isn't yet supported"),
                       virStoragePoolTypeToString(pooldef->type));
        goto cleanup;
    }

    ret = 0;
 cleanup:
    virObjectUnref(pool);
    virObjectUnref(vol);
    VIR_FREE(poolxml);
    virStoragePoolDefFree(pooldef);
    return ret;
}


/*
 * virStoragePoolObjFindPoolByUUID
 * @uuid: The uuid to lookup
 *
 * Using the passed @uuid, search the driver pools for a matching uuid.
 * If found, then lock the pool
 *
 * Returns NULL if pool is not found or a locked pool object pointer
 */
virStoragePoolObjPtr
virStoragePoolObjFindPoolByUUID(const unsigned char *uuid)
{
    virStoragePoolObjPtr obj;

    storageDriverLock();
    obj = virStoragePoolObjFindByUUID(&driver->pools, uuid);
    storageDriverUnlock();
    return obj;
}


/*
 * virStoragePoolObjBuildTempFilePath
 * @obj: pool object pointer
 * @vol: volume definition
 *
 * Generate a name for a temporary file using the driver stateDir
 * as a path, the pool name, and the volume name to be used as input
 * for a mkostemp
 *
 * Returns a string pointer on success, NULL on failure
 */
char *
virStoragePoolObjBuildTempFilePath(virStoragePoolObjPtr obj,
                                   virStorageVolDefPtr voldef)

{
    virStoragePoolDefPtr def = virStoragePoolObjGetDef(obj);
    char *tmp = NULL;

    ignore_value(virAsprintf(&tmp, "%s/%s.%s.secret.XXXXXX",
                             driver->stateDir, def->name, voldef->name));
    return tmp;
}
