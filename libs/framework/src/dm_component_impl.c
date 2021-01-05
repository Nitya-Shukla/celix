/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *  KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <uuid/uuid.h>
#include <celix_utils.h>
#include <assert.h>
#include <celix_bundle.h>

#include "celix_constants.h"
#include "celix_filter.h"
#include "dm_component_impl.h"
#include "bundle_context_private.h"
#include "framework_private.h"

struct celix_dm_component_struct {
    char uuid[DM_COMPONENT_MAX_ID_LENGTH];
    char name[DM_COMPONENT_MAX_NAME_LENGTH];

    celix_bundle_context_t* context;
    bool setCLanguageProperty;
    void* implementation;

    celix_dm_cmp_lifecycle_fpt callbackInit;
    celix_dm_cmp_lifecycle_fpt callbackStart;
    celix_dm_cmp_lifecycle_fpt callbackStop;
    celix_dm_cmp_lifecycle_fpt callbackDeinit;

    celix_thread_mutex_t mutex; //protects below
    celix_array_list_t* providedInterfaces;
    celix_array_list_t* dependencies;

    /**
     * TODO needed
     * True if the component provides the same service type as it depends on.
     * In that case a addtional check has to be made that the component does not depend on its own provided service,
     * because this can result in a deadlock (cannot unregister if service is in use and unregister can happen in a
     * remote service tracker callback)
     */
     bool providesSameServiceAsOneOfTheDependencies;

    celix_dm_component_state_t state;
    bool isStarted;
};

typedef struct dm_interface_struct {
    char* serviceName;
    const void* service;
    celix_properties_t *properties;
    long svcId;
    bool unregistering;
} dm_interface_t;

static celix_status_t component_registerServices(celix_dm_component_t *component);
static celix_status_t component_unregisterServices(celix_dm_component_t *component);
static bool component_areAllRequiredServiceDependenciesResolved(celix_dm_component_t *component);
static celix_status_t component_performTransition(celix_dm_component_t *component, celix_dm_component_state_t oldState, celix_dm_component_state_t newState, bool *transition);
static celix_status_t component_calculateNewState(celix_dm_component_t *component, celix_dm_component_state_t currentState, celix_dm_component_state_t *newState);
static celix_status_t component_handleChange(celix_dm_component_t *component);
static celix_status_t component_handleAdd(celix_dm_component_t *component, const celix_dm_event_t* event);
static celix_status_t component_handleRemove(celix_dm_component_t *component, const celix_dm_event_t* event);
static celix_status_t component_handleSet(celix_dm_component_t *component, const celix_dm_event_t* event);
static celix_status_t component_startDependencies(celix_dm_component_t *component);
static celix_status_t component_suspend(celix_dm_component_t *component, celix_dm_service_dependency_t *dependency);
static celix_status_t component_resume(celix_dm_component_t *component, celix_dm_service_dependency_t *dependency);
static celix_status_t celix_dmComponent_stop(celix_dm_component_t *component);


celix_dm_component_t* celix_dmComponent_create(bundle_context_t *context, const char* name) {
    return celix_dmComponent_createWithUUID(context, name, NULL);
}

celix_dm_component_t* celix_dmComponent_createWithUUID(bundle_context_t *context, const char* name, const char *uuidIn) {
    celix_dm_component_t *component = calloc(1, sizeof(*component));

    char uuidStr[DM_COMPONENT_MAX_ID_LENGTH];
    bool genRandomUUID = true;
    if (uuidIn != NULL) {
        uuid_t uuid;
        int rc = uuid_parse(uuidIn, uuid);
        if (rc == 0) {
            uuid_unparse(uuid, uuidStr);
            genRandomUUID = false;
        } else {
            //parsing went wrong
            fw_log(context->framework->logger, CELIX_LOG_LEVEL_WARNING,
                   "Cannot parse provided uuid '%s'. Not a valid UUID?. UUID will be generated", uuidIn);
        }
    }

    if (genRandomUUID) {
        //gen uuid
        uuid_t uuid;
        uuid_generate(uuid);
        uuid_unparse(uuid, uuidStr);
    }
    snprintf(component->uuid, DM_COMPONENT_MAX_ID_LENGTH, "%s", uuidStr);

    snprintf(component->name, DM_COMPONENT_MAX_NAME_LENGTH, "%s", name == NULL ? "n/a" : name);

    component->context = context;
    component->implementation = NULL;
    component->callbackInit = NULL;
    component->callbackStart = NULL;
    component->callbackStop = NULL;
    component->callbackDeinit = NULL;
    component->state = DM_CMP_STATE_INACTIVE;
    component->setCLanguageProperty = false;

    component->providedInterfaces = celix_arrayList_create();
    component->dependencies = celix_arrayList_create();
    celixThreadMutex_create(&component->mutex, NULL);
    component->isStarted = false;
    return component;
}

celix_status_t component_create(bundle_context_pt context, const char *name, celix_dm_component_t **out) {
    celix_status_t status = CELIX_SUCCESS;
    celix_dm_component_t *cmp = celix_dmComponent_create(context, name);
    if (cmp == NULL) {
        status = CELIX_BUNDLE_EXCEPTION;
    } else {
        *out = cmp;
    }
    return status;
}

void component_destroy(celix_dm_component_t *component) {
    celix_dmComponent_destroy(component);
}

void celix_dmComponent_destroy(celix_dm_component_t *component) {
	if (component) {
        celix_dmComponent_stop(component); //all service deregistered // all svc tracker stopped

        for (int i = 0; i < celix_arrayList_size(component->providedInterfaces); ++i) {
            dm_interface_t *interface = celix_arrayList_get(component->providedInterfaces, i);

            if (interface->properties != NULL) {
                celix_properties_destroy(interface->properties);
            }
            free(interface->serviceName);
            free(interface);
        }
        celix_arrayList_destroy(component->providedInterfaces);

        for (int i = 0; i < celix_arrayList_size(component->dependencies); ++i) {
            celix_dm_service_dependency_t* dep = celix_arrayList_get(component->dependencies, i);
            celix_dmServiceDependency_destroy(dep);
        }
        celix_arrayList_destroy(component->dependencies);
        celixThreadMutex_destroy(&component->mutex);

        free(component);
	}
}


const char* celix_dmComponent_getUUID(celix_dm_component_t* cmp) {
    return cmp->uuid;
}

celix_status_t component_addServiceDependency(celix_dm_component_t *component, celix_dm_service_dependency_t *dep) {
    return celix_dmComponent_addServiceDependency(component, dep);
}

celix_status_t celix_dmComponent_addServiceDependency(celix_dm_component_t *component, celix_dm_service_dependency_t *dep) {
    celix_status_t status = CELIX_SUCCESS;
    celix_serviceDependency_setComponent(dep, component);

    celixThreadMutex_lock(&component->mutex);
    arrayList_add(component->dependencies, dep);
    bool startDep = component->state != DM_CMP_STATE_INACTIVE;
    if (startDep) {
        celix_serviceDependency_start(dep);
    }

    celixThreadMutex_unlock(&component->mutex);

    component_handleChange(component);
    return status;
}

celix_status_t celix_dmComponent_removeServiceDependency(celix_dm_component_t *component, celix_dm_service_dependency_t *dep) {

    celixThreadMutex_lock(&component->mutex);
    arrayList_removeElement(component->dependencies, dep);
    bool stopDependency = component->state != DM_CMP_STATE_INACTIVE;
    if (stopDependency) {
        celix_serviceDependency_stop(dep);
    }
    celix_dmServiceDependency_destroy(dep);
    celixThreadMutex_unlock(&component->mutex);

    component_handleChange(component);
    return CELIX_SUCCESS;
}

celix_dm_component_state_t component_currentState(celix_dm_component_t *cmp) {
    return celix_dmComponent_currentState(cmp);
}

celix_dm_component_state_t celix_dmComponent_currentState(celix_dm_component_t *cmp) {
    return cmp->state;
}

void* component_getImplementation(celix_dm_component_t *cmp) {
    return celix_dmComponent_getImplementation(cmp);
}

void* celix_dmComponent_getImplementation(celix_dm_component_t *cmp) {
    return cmp->implementation;
}

const char* component_getName(celix_dm_component_t *cmp) {
    return celix_dmComponent_getName(cmp);
}

const char * celix_dmComponent_getName(celix_dm_component_t *cmp) {
    return cmp->name;
}

celix_status_t component_removeServiceDependency(celix_dm_component_t *component, celix_dm_service_dependency_t *dependency) {
    return celix_dmComponent_removeServiceDependency(component, dependency);
}

celix_status_t celix_private_dmComponent_start(celix_dm_component_t *component) {
    celixThreadMutex_lock(&component->mutex);
    if (!component->isStarted) {
        component->isStarted = true;
    }
    celixThreadMutex_unlock(&component->mutex);
    component_handleChange(component);
    return CELIX_SUCCESS;
}

static celix_status_t celix_dmComponent_stop(celix_dm_component_t *component) {
    celixThreadMutex_lock(&component->mutex);
    if (component->isStarted) {
        component->isStarted = false;
    }
    celixThreadMutex_unlock(&component->mutex);
    component_handleChange(component);
    return CELIX_SUCCESS;
}


celix_status_t component_setCLanguageProperty(celix_dm_component_t *component, bool setCLangProp) {
    return celix_dmComponent_setCLanguageProperty(component, setCLangProp);
}

celix_status_t celix_dmComponent_setCLanguageProperty(celix_dm_component_t *component, bool setCLangProp) {
    component->setCLanguageProperty = setCLangProp;
    return CELIX_SUCCESS;
}

celix_status_t component_addInterface(celix_dm_component_t *component, const char* serviceName, const char* serviceVersion, const void* service, properties_pt properties) {
    return celix_dmComponent_addInterface(component, serviceName, serviceVersion, service, properties);
}

celix_status_t celix_dmComponent_addInterface(celix_dm_component_t *component, const char* serviceName, const char* serviceVersion, const void* service, celix_properties_t* properties) {
    celix_status_t status = CELIX_SUCCESS;

    dm_interface_t *interface = (dm_interface_t *) calloc(1, sizeof(*interface));
    char *name = strdup(serviceName);

    if (properties == NULL) {
        properties = celix_properties_create();
    }

    if ((properties_get(properties, CELIX_FRAMEWORK_SERVICE_VERSION) == NULL) && (serviceVersion != NULL)) {
        celix_properties_set(properties, CELIX_FRAMEWORK_SERVICE_VERSION, serviceVersion);
    }

    if (component->setCLanguageProperty && properties_get(properties, CELIX_FRAMEWORK_SERVICE_LANGUAGE) == NULL) { //always set default lang to C
        celix_properties_set(properties, CELIX_FRAMEWORK_SERVICE_LANGUAGE, CELIX_FRAMEWORK_SERVICE_C_LANGUAGE);
    }

    celix_properties_set(properties, CELIX_DM_COMPONENT_UUID, (char*)component->uuid);

    if (interface && name) {
        celixThreadMutex_lock(&component->mutex);
        interface->serviceName = name;
        interface->service = service;
        interface->properties = properties;
        interface->svcId= -1L;
        celix_arrayList_add(component->providedInterfaces, interface);
        if (component->state == DM_CMP_STATE_TRACKING_OPTIONAL) {
            component_registerServices(component);
        }
        celixThreadMutex_unlock(&component->mutex);
    } else {
        free(interface);
        free(name);
        status = CELIX_ENOMEM;
    }

    return status;
}

celix_status_t component_removeInterface(celix_dm_component_t *component, const void* service) {
    return celix_dmComponent_removeInterface(component, service);
}

celix_status_t celix_dmComponent_removeInterface(celix_dm_component_t *component, const void* service) {
    celix_status_t status = CELIX_ILLEGAL_ARGUMENT;

    celixThreadMutex_lock(&component->mutex);
    int nof_interfaces = celix_arrayList_size(component->providedInterfaces);
    dm_interface_t* removedInterface = NULL;
    for (int i = 0; i < nof_interfaces; ++i) {
        dm_interface_t *interface = celix_arrayList_get(component->providedInterfaces, i);
        if (interface->service == service) {
            celix_arrayList_removeAt(component->providedInterfaces, i);
            removedInterface = interface;
            break;
        }
    }
    celixThreadMutex_unlock(&component->mutex);

    if (removedInterface != NULL) {
        celix_bundleContext_unregisterService(component->context, removedInterface->svcId);
        free(removedInterface->serviceName);
        free(removedInterface);
    }

    return status;
}

celix_status_t component_getInterfaces(celix_dm_component_t *component, celix_array_list_t **out) {
    return celix_dmComponent_getInterfaces(component, out);
}

celix_status_t celix_dmComponent_getInterfaces(celix_dm_component_t *component, celix_array_list_t **out) {
    celix_array_list_t* names = celix_arrayList_create();

    celixThreadMutex_lock(&component->mutex);
    int size = celix_arrayList_size(component->providedInterfaces);
    for (int i = 0; i < size; i += 1) {
        dm_interface_info_t* info = calloc(1, sizeof(*info));
        dm_interface_t *interface = celix_arrayList_get(component->providedInterfaces, i);
        info->name = celix_utils_strdup(interface->serviceName);
        info->properties = celix_properties_copy(interface->properties);
        celix_arrayList_add(names, info);
    }
    celixThreadMutex_unlock(&component->mutex);
    *out = names;

    return CELIX_SUCCESS;
}

celix_status_t celix_private_dmComponent_handleEvent(celix_dm_component_t *component, const celix_dm_event_t* event) {
    celix_status_t status = CELIX_SUCCESS;
    switch (event->eventType) {
        case CELIX_DM_EVENT_SVC_ADD:
            component_handleAdd(component, event);
            break;
        case CELIX_DM_EVENT_SVC_REM:
            component_handleRemove(component, event);
            break;
        case CELIX_DM_EVENT_SVC_SET:
            component_handleSet(component, event);
            break;
        default:
            break;
    }
    return status;
}

static celix_status_t component_suspend(celix_dm_component_t *component, celix_dm_service_dependency_t *dependency) {
	celix_status_t status = CELIX_SUCCESS;
	dm_service_dependency_strategy_t strategy = celix_dmServiceDependency_getStrategy(dependency);
	if (strategy == DM_SERVICE_DEPENDENCY_STRATEGY_SUSPEND &&  component->callbackStop != NULL) {
		status = component->callbackStop(component->implementation);
	}
	return status;
}

static celix_status_t component_resume(celix_dm_component_t *component, celix_dm_service_dependency_t *dependency) {
	celix_status_t status = CELIX_SUCCESS;
    dm_service_dependency_strategy_t strategy = celix_dmServiceDependency_getStrategy(dependency);
	if (strategy == DM_SERVICE_DEPENDENCY_STRATEGY_SUSPEND &&  component->callbackStop != NULL) {
		status = component->callbackStart(component->implementation);
	}
	return status;
}

static celix_status_t component_handleAdd(celix_dm_component_t *component, const celix_dm_event_t* event) {
    celixThreadMutex_lock(&component->mutex);
    switch (component->state) {
        case DM_CMP_STATE_TRACKING_OPTIONAL:
            if (celix_serviceDependency_hasAddCallback(event->dep)) { //if to prevent unneeded suspends
                component_suspend(component, event->dep);
                celix_serviceDependency_invokeAdd(event->dep, event->svc, event->props);
                component_resume(component, event->dep);
            }
            break;
        default: //DM_CMP_STATE_INACTIVE
            celix_serviceDependency_invokeAdd(event->dep, event->svc, event->props);
            break;
    }
    celixThreadMutex_unlock(&component->mutex);
    component_handleChange(component);
    return CELIX_SUCCESS;
}

static celix_status_t component_handleRemove(celix_dm_component_t *component, const celix_dm_event_t* event) {
    component_handleChange(component);
    celixThreadMutex_lock(&component->mutex);
    switch (component->state) {
        case DM_CMP_STATE_TRACKING_OPTIONAL:
            if (celix_serviceDependency_hasRemoveCallback(event->dep)) { //if to prevent unneeded suspends
                component_suspend(component, event->dep);
                celix_serviceDependency_invokeRemove(event->dep, event->svc, event->props);
                component_resume(component, event->dep);
            }
            break;
        default: //DM_CMP_STATE_INACTIVE
            celix_serviceDependency_invokeRemove(event->dep, event->svc, event->props);
            break;
    }
    celixThreadMutex_unlock(&component->mutex);
    return CELIX_SUCCESS;
}


static celix_status_t component_handleSet(celix_dm_component_t *component, const celix_dm_event_t* event) {
    if (event->svc == NULL) {
        //note set with removes a service -> update state first
        component_handleChange(component);
    }
    celixThreadMutex_lock(&component->mutex);
    switch (component->state) {
        case DM_CMP_STATE_TRACKING_OPTIONAL:
            if (celix_serviceDependency_hasSetCallback(event->dep)) { //if to prevent unneeded suspends
                component_suspend(component, event->dep);
                celix_serviceDependency_invokeSet(event->dep, event->svc, event->props);
                component_resume(component, event->dep);
            }
            break;
        default: //DM_CMP_STATE_INACTIVE
            celix_serviceDependency_invokeSet(event->dep, event->svc, event->props);
            break;
    }
    celixThreadMutex_unlock(&component->mutex);
    if (event->svc != NULL) {
        component_handleChange(component);
    }
    return CELIX_SUCCESS;
}

/**
 * perform state transition. This call should be called with the component->mutex locked.
 */
static celix_status_t component_startDependencies(celix_dm_component_t *component) {
    for (int i = 0; i < celix_arrayList_size(component->dependencies); i++) {
        celix_dm_service_dependency_t *dependency = arrayList_get(component->dependencies, i);
        if (!celix_dmServiceDependency_isTrackerOpen(dependency)) {
            celix_serviceDependency_start(dependency);
        }
    }
    return CELIX_SUCCESS;
}

/**
 * perform state transition. This call should be called with the component->mutex locked.
 */
static celix_status_t component_stopDependencies(celix_dm_component_t *component) {
    for (int i = 0; i < celix_arrayList_size(component->dependencies); i++) {
        celix_dm_service_dependency_t *dependency = arrayList_get(component->dependencies, i);
        if (celix_dmServiceDependency_isTrackerOpen(dependency)) {
            celix_serviceDependency_stop(dependency);
        }
    }
    return CELIX_SUCCESS;
}


/**
 * Calculate and handle state change. This call should be called with the component->mutex locked.
 */
static void component_handleChangeOnEventThread(void *data) {
    celix_dm_component_t* component = data;
    assert(celix_framework_isCurrentThreadTheEventLoop(component->context->framework));

    celixThreadMutex_lock(&component->mutex);
    celix_dm_component_state_t oldState;
    celix_dm_component_state_t newState;
    bool transition = false;
    do {
        oldState = component->state;
        celix_status_t status = component_calculateNewState(component, oldState, &newState);
        if (status == CELIX_SUCCESS) {
            status = component_performTransition(component, oldState, newState, &transition);
            component->state = newState;
        }

        if (status != CELIX_SUCCESS) {
            break;
        }
    } while (transition);
    celixThreadMutex_unlock(&component->mutex);
}

static celix_status_t component_handleChange(celix_dm_component_t *component) {
    if (celix_framework_isCurrentThreadTheEventLoop(component->context->framework)) {
        component_handleChangeOnEventThread(component);
    } else {
        long eventId = celix_framework_fireGenericEvent(
                component->context->framework,
                -1,
                celix_bundle_getId(component->context->bundle),
                "dm component handle change",
                component,
                component_handleChangeOnEventThread,
                NULL,
                NULL);
        celix_framework_waitForGenericEvent(component->context->framework, eventId);
    }
    return CELIX_SUCCESS;
}


///**
// * Check if service trackers are still open for the dependencies.
// * This call should be called with the component->mutex locked.
// */
//static bool component_allDependencyStopped(celix_dm_component_t* component) {
//    bool anyOpen = false;
//    for (int i = 0; i < celix_arrayList_size(component->dependencies); ++i) {
//        celix_dm_service_dependency_t *dep = celix_arrayList_get(component->dependencies, i);
//        if (celix_dmServiceDependency_isTrackerOpen(dep)) {
//            anyOpen = true;
//            break;
//        }
//    }
//    return !anyOpen;
//}

/**
 * Calculate possible state change. This call should be called with the component->mutex locked.
 */
static celix_status_t component_calculateNewState(celix_dm_component_t *component, celix_dm_component_state_t currentState, celix_dm_component_state_t *newState) {
    celix_status_t status = CELIX_SUCCESS;

    bool allResolved = component_areAllRequiredServiceDependenciesResolved(component);
    if (currentState == DM_CMP_STATE_INACTIVE) {
        if (component->isStarted) {
            *newState = DM_CMP_STATE_WAITING_FOR_REQUIRED;
        } else {
            *newState = currentState;
        }
    } else if (currentState == DM_CMP_STATE_WAITING_FOR_REQUIRED) {
        if (!component->isStarted) {
            *newState = DM_CMP_STATE_INACTIVE;
        } else {
            if (allResolved) {
                *newState = DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED;
            } else {
                *newState = currentState;
            }
        }
    } else if (currentState == DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED) {
        if (!component->isStarted) {
            *newState = DM_CMP_STATE_WAITING_FOR_REQUIRED;
        } else {
            if (allResolved) {
                *newState = DM_CMP_STATE_TRACKING_OPTIONAL;
            } else {
                *newState = currentState;
            }
        }
    } else if (currentState == DM_CMP_STATE_TRACKING_OPTIONAL) {
        if (component->isStarted && allResolved) {
            *newState = currentState;
        } else {
            *newState = DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED;
        }
    } else {
        //should not reach
        *newState = DM_CMP_STATE_INACTIVE;
        status = CELIX_BUNDLE_EXCEPTION;
    }

    return status;
}

/**
 * perform state transition. This call should be called with the component->mutex locked.
 */
static celix_status_t component_performTransition(celix_dm_component_t *component, celix_dm_component_state_t oldState, celix_dm_component_state_t newState, bool *transition) {
    celix_status_t status = CELIX_SUCCESS;
    fw_log(component->context->framework->logger, CELIX_LOG_LEVEL_TRACE, "performing transition for component %s from state %i to state %i\n", component->name, oldState, newState);

    if (oldState == newState) {
        *transition = false;
    } else if (oldState == DM_CMP_STATE_INACTIVE && newState == DM_CMP_STATE_WAITING_FOR_REQUIRED) {
        component_startDependencies(component);
        *transition = true;
    } else if (oldState == DM_CMP_STATE_WAITING_FOR_REQUIRED && newState == DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED) {
        if (component->callbackInit) {
        	status = component->callbackInit(component->implementation);
        }
        *transition = true;
    } else if (oldState == DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED && newState == DM_CMP_STATE_TRACKING_OPTIONAL) {
        if (component->callbackStart) {
        	status = component->callbackStart(component->implementation);
        }
        component_registerServices(component);
        *transition = true;
    } else if (oldState == DM_CMP_STATE_TRACKING_OPTIONAL && newState == DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED) {
        component_unregisterServices(component);
        if (component->callbackStop) {
        	status = component->callbackStop(component->implementation);
        }
        *transition = true;
    } else if (oldState == DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED && newState == DM_CMP_STATE_WAITING_FOR_REQUIRED) {
        if (component->callbackDeinit) {
            status = component->callbackDeinit(component->implementation);
        }
        *transition = true;
    } else if (oldState == DM_CMP_STATE_WAITING_FOR_REQUIRED && newState == DM_CMP_STATE_INACTIVE) {
        component_stopDependencies(component);
        *transition = true;
    }

    return status;
}

/**
 * Check if all required dependencies are resolved. This call should be called with the component->mutex locked.
 */
static bool component_areAllRequiredServiceDependenciesResolved(celix_dm_component_t *component) {
    bool allResolved = true;
    for (int i = 0; i < celix_arrayList_size(component->dependencies); i++) {
        celix_dm_service_dependency_t *dependency = celix_arrayList_get(component->dependencies, i);
        bool started = celix_dmServiceDependency_isTrackerOpen(dependency);
        bool required = celix_dmServiceDependency_isRequired(dependency);
        bool available = celix_serviceDependency_isAvailable(dependency);
        if (!started) {
            allResolved = false;
            break;
        }
        if (required && !available) {
            allResolved = false;
            break;
        }
    }
    return allResolved;
}

/**
 * Register component services (if not already registered). This call should be called with the component->mutex locked.
 */
static celix_status_t component_registerServices(celix_dm_component_t *component) {
    for (int i = 0; i < celix_arrayList_size(component->providedInterfaces); i++) {
        dm_interface_t *interface = arrayList_get(component->providedInterfaces, i);
        if (interface->svcId == -1L) {
            celix_properties_t *regProps = celix_properties_copy(interface->properties);
            celix_service_registration_options_t opts = CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
            opts.properties = regProps;
            opts.svc = (void*)interface->service;
            opts.serviceName = interface->serviceName;
            opts.serviceLanguage = celix_properties_get(regProps, CELIX_FRAMEWORK_SERVICE_LANGUAGE, NULL);
            interface->svcId = celix_bundleContext_registerServiceWithOptionsAsync(component->context, &opts);
            if (!celix_framework_isCurrentThreadTheEventLoop(component->context->framework)) {
                celix_framework_waitForAsyncRegistration(component->context->framework, interface->svcId);
            }
        }
    }

    return CELIX_SUCCESS;
}

/**
 * Unregister component services. This call should be called with the component->mutex locked.
 */
static celix_status_t component_unregisterServices(celix_dm_component_t *component) {
    celix_status_t status = CELIX_SUCCESS;

    celix_array_list_t* ids = celix_arrayList_create();
    for (int i = 0; i < celix_arrayList_size(component->providedInterfaces); ++i) {
	    dm_interface_t *interface = arrayList_get(component->providedInterfaces, i);
	    interface->unregistering = true;
	    interface->svcId = -1L;
    }

    celixThreadMutex_unlock(&component->mutex);
    for (int i = 0; i < celix_arrayList_size(ids); ++i) {
        long svcId = celix_arrayList_getLong(ids, i);
        celix_bundleContext_unregisterService(component->context, svcId); //TODO make async
    }
    celixThreadMutex_lock(&component->mutex);
    celix_arrayList_destroy(ids);

    return status;
}

celix_status_t component_setCallbacks(celix_dm_component_t *component, celix_dm_cmp_lifecycle_fpt init, celix_dm_cmp_lifecycle_fpt start, celix_dm_cmp_lifecycle_fpt stop, celix_dm_cmp_lifecycle_fpt deinit) {
    return celix_dmComponent_setCallbacks(component, init, start, stop, deinit);
}

celix_status_t celix_dmComponent_setCallbacks(celix_dm_component_t *component, celix_dm_cmp_lifecycle_fpt init, celix_dm_cmp_lifecycle_fpt start, celix_dm_cmp_lifecycle_fpt stop, celix_dm_cmp_lifecycle_fpt deinit) {
	component->callbackInit = init;
	component->callbackStart = start;
	component->callbackStop = stop;
	component->callbackDeinit = deinit;
	return CELIX_SUCCESS;
}

celix_status_t component_setImplementation(celix_dm_component_t *component, void *implementation) {
    return celix_dmComponent_setImplementation(component, implementation);
}

celix_status_t celix_dmComponent_setImplementation(celix_dm_component_t *component, void *implementation) {
    component->implementation = implementation;
    return CELIX_SUCCESS;
}

celix_status_t component_getBundleContext(celix_dm_component_t *component, bundle_context_pt *context) {
    *context = celix_dmComponent_getBundleContext(component);
    return *context == NULL ? CELIX_BUNDLE_EXCEPTION : CELIX_SUCCESS;
}

celix_bundle_context_t* celix_dmComponent_getBundleContext(celix_dm_component_t *component) {
	celix_bundle_context_t *result = NULL;
	if (component) {
		result = component->context;
	}
    return result;
}

celix_status_t component_getComponentInfo(celix_dm_component_t *component, dm_component_info_pt *out) {
    return celix_dmComponent_getComponentInfo(component, out);
}

celix_status_t celix_dmComponent_getComponentInfo(celix_dm_component_t *component, dm_component_info_pt *out) {
    dm_component_info_pt info = calloc(1, sizeof(*info));
    info->dependency_list = celix_arrayList_create();
    celix_dmComponent_getInterfaces(component, &info->interfaces);

    celixThreadMutex_lock(&component->mutex);
    info->active = false;
    memcpy(info->id, component->uuid, DM_COMPONENT_MAX_ID_LENGTH);
    memcpy(info->name, component->name, DM_COMPONENT_MAX_NAME_LENGTH);

    switch (component->state) {
        case DM_CMP_STATE_INACTIVE :
            info->state = strdup("INACTIVE");
            break;
        case DM_CMP_STATE_WAITING_FOR_REQUIRED :
            info->state = strdup("WAITING_FOR_REQUIRED");
            break;
        case DM_CMP_STATE_INSTANTIATED_AND_WAITING_FOR_REQUIRED :
            info->state = strdup("INSTANTIATED_AND_WAITING_FOR_REQUIRED");
            break;
        case DM_CMP_STATE_TRACKING_OPTIONAL :
            info->state = strdup("TRACKING_OPTIONAL");
            info->active = true;
            break;
        default :
            info->state = strdup("UNKNOWN");
            break;
    }

    for (int i = 0; i < celix_arrayList_size(component->dependencies); i += 1) {
        celix_dm_service_dependency_t *dep = celix_arrayList_get(component->dependencies, i);
        dm_service_dependency_info_pt depInfo = celix_dmServiceDependency_createInfo(dep);
        celix_arrayList_add(info->dependency_list, depInfo);
    }

    celixThreadMutex_unlock(&component->mutex);

    *out = info;
    return CELIX_SUCCESS;
}
void component_destroyComponentInfo(dm_component_info_pt info) {
    return celix_dmComponent_destroyComponentInfo(info);
}

void celix_dmComponent_destroyComponentInfo(dm_component_info_pt info) {
    int i;
    int size;
    if (info != NULL) {
        free(info->state);

        if (info->interfaces != NULL) {
            size = celix_arrayList_size(info->interfaces);
            for (i = 0; i < size; i += 1) {
                dm_interface_info_pt intfInfo = celix_arrayList_get(info->interfaces, i);
                free(intfInfo->name);
                properties_destroy(intfInfo->properties);
                free(intfInfo);
            }
            celix_arrayList_destroy(info->interfaces);
        }
        if (info->dependency_list != NULL) {
            size = celix_arrayList_size(info->dependency_list);
            for (i = 0; i < size; i += 1) {
                dm_service_dependency_info_pt depInfo = celix_arrayList_get(info->dependency_list, i);
                dependency_destroyDependencyInfo(depInfo);
            }
            celix_arrayList_destroy(info->dependency_list);
        }
    }
    free(info);
}

bool celix_dmComponent_isActive(celix_dm_component_t *component) {
    celixThreadMutex_lock(&component->mutex);
    bool active = component->state == DM_CMP_STATE_TRACKING_OPTIONAL;
    celixThreadMutex_unlock(&component->mutex);
    return active;
}
