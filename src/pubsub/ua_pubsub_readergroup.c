/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) 2017-2018 Fraunhofer IOSB (Author: Andreas Ebner)
 * Copyright (c) 2019 Fraunhofer IOSB (Author: Julius Pfrommer)
 * Copyright (c) 2019 Kalycito Infotech Private Limited
 * Copyright (c) 2021 Fraunhofer IOSB (Author: Jan Hermes)
 * Copyright (c) 2022 Linutronix GmbH (Author: Muddasir Shakil)
 *
 */

#include "ua_pubsub.h"
#include "server/ua_server_internal.h"

#ifdef UA_ENABLE_PUBSUB /* conditional compilation */

#ifdef UA_ENABLE_PUBSUB_INFORMATIONMODEL
#include "ua_pubsub_networkmessage.h"
#include "ua_pubsub_ns0.h"
#endif

UA_ReaderGroup *
UA_ReaderGroup_findRGbyId(UA_Server *server, UA_NodeId identifier) {
    UA_PubSubConnection *pubSubConnection;
    TAILQ_FOREACH(pubSubConnection, &server->pubSubManager.connections, listEntry){
        UA_ReaderGroup* readerGroup = NULL;
        LIST_FOREACH(readerGroup, &pubSubConnection->readerGroups, listEntry) {
            if(UA_NodeId_equal(&identifier, &readerGroup->identifier))
                return readerGroup;
        }
    }
    return NULL;
}

UA_DataSetReader *
UA_ReaderGroup_findDSRbyId(UA_Server *server, UA_NodeId identifier) {
    UA_PubSubConnection *pubSubConnection;
    TAILQ_FOREACH(pubSubConnection, &server->pubSubManager.connections, listEntry){
        UA_ReaderGroup* readerGroup = NULL;
        LIST_FOREACH(readerGroup, &pubSubConnection->readerGroups, listEntry) {
            UA_DataSetReader *tmpReader;
            LIST_FOREACH(tmpReader, &readerGroup->readers, listEntry) {
                if(UA_NodeId_equal(&tmpReader->identifier, &identifier))
                    return tmpReader;
            }
        }
    }
    return NULL;
}

/* ReaderGroup Config Handling */

UA_StatusCode
UA_ReaderGroupConfig_copy(const UA_ReaderGroupConfig *src,
                          UA_ReaderGroupConfig *dst) {
    memcpy(dst, src, sizeof(UA_ReaderGroupConfig));
    UA_StatusCode res = UA_STATUSCODE_GOOD;
    res |= UA_String_copy(&src->name, &dst->name);
    res |= UA_KeyValueMap_copy(&src->groupProperties, &dst->groupProperties);
#ifdef UA_ENABLE_PUBSUB_ENCRYPTION
    res = UA_String_copy(&src->securityGroupId, &dst->securityGroupId);
#endif
    if(res != UA_STATUSCODE_GOOD)
        UA_ReaderGroupConfig_clear(dst);
    return res;
}

void
UA_ReaderGroupConfig_clear(UA_ReaderGroupConfig *readerGroupConfig) {
    UA_String_clear(&readerGroupConfig->name);
    UA_KeyValueMap_clear(&readerGroupConfig->groupProperties);
#ifdef UA_ENABLE_PUBSUB_ENCRYPTION
    UA_String_clear(&readerGroupConfig->securityGroupId);
#endif
}

/* This triggers the collection and reception of NetworkMessages and the
 * contained DataSetMessages. */
static void
subscribeCallback(UA_Server *server,
                  UA_ReaderGroup *readerGroup) {
    // TODO: feedback for debug-assert vs runtime-check
    UA_assert(server);
    UA_assert(readerGroup);

    UA_LOG_DEBUG_READERGROUP(&server->config.logger, readerGroup,
                             "PubSub subscribe callback");

    UA_PubSubConnection *connection = readerGroup->linkedConnection;
    if(!connection) {
        UA_LOG_ERROR_READERGROUP(&server->config.logger, readerGroup,
                     "SubscribeCallback(): Find linked connection failed");
        UA_ReaderGroup_setPubSubState(server, readerGroup, UA_PUBSUBSTATE_ERROR,
                                      UA_STATUSCODE_BADCONNECTIONCLOSED);
        return;
    }

    receiveBufferedNetworkMessage(server, readerGroup, connection);
}

static void
UA_ReaderGroup_subscribeCallback(UA_Server *server,
                                 UA_ReaderGroup *readerGroup) {
    UA_LOCK(&server->serviceMutex);
    subscribeCallback(server, readerGroup);
    UA_UNLOCK(&server->serviceMutex);
}

/* ReaderGroup Lifecycle */

/* Add new subscribeCallback. The first execution is triggered directly after
 * creation. */
static UA_StatusCode
UA_ReaderGroup_addSubscribeCallback(UA_Server *server, UA_ReaderGroup *readerGroup) {
    /* Already registered */
    if(readerGroup->subscribeCallbackId != 0)
        return UA_STATUSCODE_BADINTERNALERROR;

    UA_EventLoop *el = readerGroup->linkedConnection->config.eventLoop;
    if(!el)
        el = server->config.eventLoop;

    UA_StatusCode retval =
        el->addCyclicCallback(el, (UA_Callback)UA_ReaderGroup_subscribeCallback,
                              server, readerGroup,
                              readerGroup->config.subscribingInterval,
                              NULL /* TODO: use basetime */,
                              UA_TIMER_HANDLE_CYCLEMISS_WITH_CURRENTTIME /* TODO: Send
                                                                          * timer policy
                                                                          * from writer
                                                                          * group
                                                                          * config */,
                              &readerGroup->subscribeCallbackId);
    if(retval != UA_STATUSCODE_GOOD)
        return retval;

    /* Run once after creation */
    /* When using blocking socket functionality, the server mechanism might get
     * blocked. It is highly recommended to use custom callback when using
     * blockingsocket. */
    if(readerGroup->config.enableBlockingSocket != true)
        subscribeCallback(server, readerGroup);

    return retval;
}

static void
UA_ReaderGroup_removeSubscribeCallback(UA_Server *server,
                                       UA_ReaderGroup *readerGroup) {
    UA_PubSubConnection *connection = readerGroup->linkedConnection;
    UA_EventLoop *el = server->config.eventLoop;
    if(connection && connection->config.eventLoop)
        el = connection->config.eventLoop;
    if(readerGroup->subscribeCallbackId != 0)
        el->removeCyclicCallback(el, readerGroup->subscribeCallbackId);
    readerGroup->subscribeCallbackId = 0;
}

UA_StatusCode
UA_ReaderGroup_create(UA_Server *server, UA_NodeId connectionId,
                      const UA_ReaderGroupConfig *rgc,
                      UA_NodeId *readerGroupId) {
    /* Check for valid readergroup configuration */
    if(!rgc)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    /* Search the connection by the given connectionIdentifier */
    UA_PubSubConnection *connection =
        UA_PubSubConnection_findConnectionbyId(server, connectionId);
    if(!connection)
        return UA_STATUSCODE_BADNOTFOUND;

    if(!rgc->pubsubManagerCallback.addCustomCallback &&
       rgc->enableBlockingSocket) {
        UA_LOG_WARNING_CONNECTION(&server->config.logger, connection,
                                  "Adding ReaderGroup failed, blocking socket "
                                  "functionality only supported in customcallback");
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    if(connection->configurationFreezeCounter > 0) {
        UA_LOG_WARNING_CONNECTION(&server->config.logger, connection,
                                  "Adding ReaderGroup failed. "
                                  "Connection configuration is frozen.");
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    /* Regist (bind) the connection channel if it is not already registered */
    UA_StatusCode retval = UA_STATUSCODE_GOOD;
    if(!connection->isRegistered) {
        retval |= UA_PubSubConnection_regist(server, &connectionId, rgc);
        if(retval != UA_STATUSCODE_GOOD)
            return retval;
    }

    /* Allocate memory for new reader group */
    UA_ReaderGroup *newGroup = (UA_ReaderGroup *)UA_calloc(1, sizeof(UA_ReaderGroup));
    if(!newGroup)
        return UA_STATUSCODE_BADOUTOFMEMORY;

    newGroup->componentType = UA_PUBSUB_COMPONENT_READERGROUP;

    /* Deep copy of the config */
    retval = UA_ReaderGroupConfig_copy(rgc, &newGroup->config);
    if(retval != UA_STATUSCODE_GOOD) {
        UA_free(newGroup);
        return retval;
    }

    /* Check user configured params and define it accordingly */
    if(newGroup->config.subscribingInterval <= 0.0)
        newGroup->config.subscribingInterval = 5; /* Set default to 5 ms */

    if(newGroup->config.enableBlockingSocket)
        newGroup->config.timeout = 0; /* Set timeout to 0 for blocking socket */

    if((!newGroup->config.enableBlockingSocket) && (!newGroup->config.timeout))
        newGroup->config.timeout = 1000; /* Set default to 1ms socket timeout
                                          * when non-blocking socket allows with
                                          * zero timeout */

    newGroup->linkedConnection = connection;

    /* Add to the connection */
    LIST_INSERT_HEAD(&connection->readerGroups, newGroup, listEntry);
    connection->readerGroupsSize++;

#ifdef UA_ENABLE_PUBSUB_INFORMATIONMODEL
    retval |= addReaderGroupRepresentation(server, newGroup);
#else
    UA_PubSubManager_generateUniqueNodeId(&server->pubSubManager,
                                          &newGroup->identifier);
#endif

#ifdef UA_ENABLE_PUBSUB_SKS
    if(rgc->securityMode == UA_MESSAGESECURITYMODE_SIGN ||
       rgc->securityMode == UA_MESSAGESECURITYMODE_SIGNANDENCRYPT) {
        if(!UA_String_isEmpty(&rgc->securityGroupId) && rgc->securityPolicy) {
            /* Does the key storage already exist? */
            newGroup->keyStorage =
                UA_PubSubKeyStorage_findKeyStorage(server, rgc->securityGroupId);

            if(!newGroup->keyStorage) {
                /* Create a new key storage */
                newGroup->keyStorage = (UA_PubSubKeyStorage *)
                    UA_calloc(1, sizeof(UA_PubSubKeyStorage));
                if(!newGroup->keyStorage)
                    return UA_STATUSCODE_BADOUTOFMEMORY;
                retval = UA_PubSubKeyStorage_init(server, newGroup->keyStorage,
                                                  &rgc->securityGroupId,
                                                  rgc->securityPolicy, 0, 0);
                if(retval != UA_STATUSCODE_GOOD) {
                    UA_ReaderGroupConfig_clear(&newGroup->config);
                    UA_free(newGroup);
                    return retval;
                }
            }

            /* Increase the ref count */
            newGroup->keyStorage->referenceCount++;
        }
    }

#endif

    if(readerGroupId)
        UA_NodeId_copy(&newGroup->identifier, readerGroupId);

    /* Set the assigment between ReaderGroup and Topic if the transport layer is MQTT. */
    const UA_String transport_uri =
        UA_STRING("http://opcfoundation.org/UA-Profile/Transport/pubsub-mqtt");
    if(UA_String_equal(&connection->config.transportProfileUri, &transport_uri)) {
        UA_String topic = ((UA_BrokerWriterGroupTransportDataType *)
                           rgc->transportSettings.content.decoded.data)->queueName;
        retval |= UA_PubSubManager_addPubSubTopicAssign(server, newGroup, topic);
    }
    return retval;
}

UA_StatusCode
UA_Server_addReaderGroup(UA_Server *server, UA_NodeId connectionIdentifier,
                         const UA_ReaderGroupConfig *readerGroupConfig,
                         UA_NodeId *readerGroupIdentifier) {
    UA_LOCK(&server->serviceMutex);
    UA_StatusCode res =
        UA_ReaderGroup_create(server, connectionIdentifier,
                              readerGroupConfig, readerGroupIdentifier);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}

UA_StatusCode
UA_ReaderGroup_remove(UA_Server *server, UA_ReaderGroup *rg) {
    if(rg->configurationFrozen) {
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg,
                                   "Remove ReaderGroup failed. "
                                   "Subscriber configuration is frozen.");
        return UA_STATUSCODE_BADCONFIGURATIONERROR;
    }

    /* Unregister subscribe callback */
    if(rg->state == UA_PUBSUBSTATE_OPERATIONAL)
        UA_ReaderGroup_removeSubscribeCallback(server, rg);

#ifdef UA_ENABLE_PUBSUB_INFORMATIONMODEL
    deleteNode(server, rg->identifier, true);
#endif

    UA_DataSetReader *dsr, *tmp_dsr;
    LIST_FOREACH_SAFE(dsr, &rg->readers, listEntry, tmp_dsr) {
        UA_DataSetReader_remove(server, dsr);
    }

#ifdef UA_ENABLE_PUBSUB_ENCRYPTION
    if(rg->config.securityPolicy && rg->securityPolicyContext) {
        rg->config.securityPolicy->deleteContext(rg->securityPolicyContext);
        rg->securityPolicyContext = NULL;
    }
#endif

#ifdef UA_ENABLE_PUBSUB_SKS
    if(rg->keyStorage) {
        UA_PubSubKeyStorage_detachKeyStorage(server, rg->keyStorage);
        rg->keyStorage = NULL;
    }
#endif

    LIST_REMOVE(rg, listEntry);
    UA_PubSubConnection* pConn = rg->linkedConnection;
    if(pConn != NULL)
        pConn->readerGroupsSize--;

    UA_NodeId_clear(&rg->identifier);
    UA_ReaderGroupConfig_clear(&rg->config);
    UA_free(rg);

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_Server_removeReaderGroup(UA_Server *server, UA_NodeId groupIdentifier) {
    UA_LOCK(&server->serviceMutex);
    UA_ReaderGroup* readerGroup =
        UA_ReaderGroup_findRGbyId(server, groupIdentifier);
    if(!readerGroup) {
        UA_UNLOCK(&server->serviceMutex);
        return UA_STATUSCODE_BADNOTFOUND;
    }
    UA_StatusCode res = UA_ReaderGroup_remove(server, readerGroup);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}

UA_StatusCode
UA_Server_ReaderGroup_getConfig(UA_Server *server, UA_NodeId readerGroupIdentifier,
                                UA_ReaderGroupConfig *config) {
    if(!config)
        return UA_STATUSCODE_BADINVALIDARGUMENT;

    UA_LOCK(&server->serviceMutex);

    /* Identify the readergroup through the readerGroupIdentifier */
    UA_ReaderGroup *currentReaderGroup =
        UA_ReaderGroup_findRGbyId(server, readerGroupIdentifier);
    if(!currentReaderGroup) {
        UA_UNLOCK(&server->serviceMutex);
        return UA_STATUSCODE_BADNOTFOUND;
    }

    UA_StatusCode ret =
        UA_ReaderGroupConfig_copy(&currentReaderGroup->config, config);

    UA_UNLOCK(&server->serviceMutex);
    return ret;
}

UA_StatusCode
UA_Server_ReaderGroup_getState(UA_Server *server, UA_NodeId readerGroupIdentifier,
                               UA_PubSubState *state) {
    if((server == NULL) || (state == NULL))
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    UA_LOCK(&server->serviceMutex);
    UA_StatusCode ret = UA_STATUSCODE_BADNOTFOUND;
    UA_ReaderGroup *rg =
        UA_ReaderGroup_findRGbyId(server, readerGroupIdentifier);
    if(rg) {
        *state = rg->state;
        ret = UA_STATUSCODE_GOOD;
    }
    UA_UNLOCK(&server->serviceMutex);
    return ret;
}

/* ReaderGroup State */

static UA_StatusCode
UA_ReaderGroup_setPubSubState_disable(UA_Server *server,
                                      UA_ReaderGroup *rg,
                                      UA_StatusCode cause) {
    UA_DataSetReader *dataSetReader;
    switch(rg->state) {
    case UA_PUBSUBSTATE_DISABLED:
        return UA_STATUSCODE_GOOD;
    case UA_PUBSUBSTATE_PAUSED:
        break;
    case UA_PUBSUBSTATE_OPERATIONAL:
    case UA_PUBSUBSTATE_PREOPERATIONAL:
        UA_ReaderGroup_removeSubscribeCallback(server, rg);
        LIST_FOREACH(dataSetReader, &rg->readers, listEntry) {
            UA_DataSetReader_setPubSubState(server, dataSetReader,
                                            UA_PUBSUBSTATE_DISABLED, cause);
        }
        rg->state = UA_PUBSUBSTATE_DISABLED;
        break;
    case UA_PUBSUBSTATE_ERROR:
        break;
    default:
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg,
                                   "Unknown PubSub state!");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode
UA_ReaderGroup_setPubSubState_paused(UA_Server *server,
                                     UA_ReaderGroup *rg,
                                     UA_StatusCode cause) {
    UA_LOG_DEBUG_READERGROUP(&server->config.logger, rg,
                             "PubSub state paused is unsupported at the moment!");
    (void)cause;
    switch(rg->state) {
    case UA_PUBSUBSTATE_DISABLED:
        rg->state = UA_PUBSUBSTATE_PAUSED;
        return UA_STATUSCODE_GOOD;
    case UA_PUBSUBSTATE_PAUSED:
        return UA_STATUSCODE_GOOD;
    case UA_PUBSUBSTATE_OPERATIONAL:
    case UA_PUBSUBSTATE_PREOPERATIONAL:
        break;
    case UA_PUBSUBSTATE_ERROR:
        break;
    default:
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg, "Unknown PubSub state!");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_BADNOTSUPPORTED;
}

static UA_StatusCode
UA_ReaderGroup_setPubSubState_preoperational(UA_Server *server,
                                            UA_ReaderGroup *rg,
                                            UA_StatusCode cause) {
    switch(rg->state) {
        case UA_PUBSUBSTATE_DISABLED:
        case UA_PUBSUBSTATE_PAUSED:
            rg->state = UA_PUBSUBSTATE_PREOPERATIONAL;
            UA_ReaderGroup_addSubscribeCallback(server, rg);
            return UA_STATUSCODE_GOOD;
        case UA_PUBSUBSTATE_PREOPERATIONAL:
            return UA_STATUSCODE_GOOD;
        case UA_PUBSUBSTATE_OPERATIONAL:
            return UA_STATUSCODE_GOOD;
        case UA_PUBSUBSTATE_ERROR:
            break;
        default:
            UA_LOG_WARNING(&server->config.logger, UA_LOGCATEGORY_SERVER,
                           "Unknown PubSub state!");
            return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_BADNOTSUPPORTED;
}

static UA_StatusCode
UA_ReaderGroup_setPubSubState_operational(UA_Server *server,
                                          UA_ReaderGroup *rg,
                                          UA_StatusCode cause) {
    UA_PubSubState state = UA_PUBSUBSTATE_OPERATIONAL;
    UA_DataSetReader *dataSetReader = LIST_FIRST(&rg->readers);
    if(!dataSetReader ||
       (rg->config.rtLevel == UA_PUBSUB_RT_FIXED_SIZE &&
        rg->configurationFrozen && !dataSetReader->bufferedMessage.nm))
        state = UA_PUBSUBSTATE_PREOPERATIONAL;

    UA_PubSubState oldstate = rg->state;
    rg->state = state; /* Set the new state now. So we can already switch from
                        * pre-operational to operational when the first messages
                        * are received during the enabled-calback step. */

    switch(oldstate) {
    case UA_PUBSUBSTATE_DISABLED:
       return UA_STATUSCODE_BADNOTSUPPORTED;
    case UA_PUBSUBSTATE_OPERATIONAL:
    case UA_PUBSUBSTATE_PREOPERATIONAL:
        LIST_FOREACH(dataSetReader, &rg->readers, listEntry) {
            UA_DataSetReader_setPubSubState(server, dataSetReader, state, cause);
        }
        return UA_STATUSCODE_GOOD;
    case UA_PUBSUBSTATE_ERROR:
        break;
    default:
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg, "Unknown PubSub state!");
        rg->state = UA_PUBSUBSTATE_ERROR;
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_BADNOTSUPPORTED;
}

static UA_StatusCode
UA_ReaderGroup_setPubSubState_error(UA_Server *server,
                                    UA_ReaderGroup *rg,
                                    UA_StatusCode cause) {
    UA_DataSetReader *dataSetReader;
    switch(rg->state) {
    case UA_PUBSUBSTATE_DISABLED:
        break;
    case UA_PUBSUBSTATE_PAUSED:
        break;
    case UA_PUBSUBSTATE_OPERATIONAL:
    case UA_PUBSUBSTATE_PREOPERATIONAL:
        UA_ReaderGroup_removeSubscribeCallback(server, rg);
        LIST_FOREACH(dataSetReader, &rg->readers, listEntry){
            UA_DataSetReader_setPubSubState(server, dataSetReader, UA_PUBSUBSTATE_ERROR,
                                            cause);
        }
        break;
    case UA_PUBSUBSTATE_ERROR:
        return UA_STATUSCODE_GOOD;
    default:
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg, "Unknown PubSub state!");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    rg->state = UA_PUBSUBSTATE_ERROR;
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_ReaderGroup_setPubSubState(UA_Server *server,
                              UA_ReaderGroup *readerGroup,
                              UA_PubSubState state,
                              UA_StatusCode cause) {
    UA_LOCK_ASSERT(&server->serviceMutex, 1);

    UA_StatusCode ret = UA_STATUSCODE_BADINVALIDARGUMENT;
    UA_PubSubState oldState = readerGroup->state;
    switch(state) {
        case UA_PUBSUBSTATE_DISABLED:
            ret = UA_ReaderGroup_setPubSubState_disable(server, readerGroup, cause);
            break;
        case UA_PUBSUBSTATE_PAUSED:
            ret = UA_ReaderGroup_setPubSubState_paused(server, readerGroup, cause);
            break;
        case UA_PUBSUBSTATE_PREOPERATIONAL:
            ret = UA_ReaderGroup_setPubSubState_preoperational(server, readerGroup, cause);
            break;
        case UA_PUBSUBSTATE_OPERATIONAL:
            ret = UA_ReaderGroup_setPubSubState_operational(server, readerGroup, cause);
            break;
        case UA_PUBSUBSTATE_ERROR:
            ret = UA_ReaderGroup_setPubSubState_error(server, readerGroup, cause);
            break;
        default:
            UA_LOG_WARNING_READERGROUP(&server->config.logger, readerGroup,
                                       "Received unknown PubSub state!");
            break;
    }
    if(state != oldState) {
        /* inform application about state change */
        UA_ServerConfig *pConfig = &server->config;
        if(pConfig->pubSubConfig.stateChangeCallback != 0) {
            pConfig->pubSubConfig.
                stateChangeCallback(server, &readerGroup->identifier, readerGroup->state, cause);
        }
    }
    return ret;
}

#ifdef UA_ENABLE_PUBSUB_SKS
UA_StatusCode
UA_Server_setReaderGroupActivateKey(UA_Server *server,
                                    const UA_NodeId readerGroupId) {
    UA_LOCK(&server->serviceMutex);
    UA_StatusCode ret = UA_STATUSCODE_BADNOTFOUND;
    UA_ReaderGroup *rg = UA_ReaderGroup_findRGbyId(server, readerGroupId);
    if(rg) {

        if(rg->keyStorage && rg->keyStorage->currentItem) {
            UA_StatusCode retval = UA_PubSubKeyStorage_activateKeyToChannelContext(
                server, rg->identifier, rg->config.securityGroupId);
            if(retval != UA_STATUSCODE_GOOD) {
                UA_UNLOCK(&server->serviceMutex);
                return retval;
            }
        }

    }
    UA_UNLOCK(&server->serviceMutex);
    return ret;
}
#endif

UA_StatusCode
UA_Server_enableReaderGroup(UA_Server *server, const UA_NodeId readerGroupId){
    UA_LOCK(&server->serviceMutex);
    UA_StatusCode ret = UA_STATUSCODE_BADNOTFOUND;
    UA_ReaderGroup *rg = UA_ReaderGroup_findRGbyId(server, readerGroupId);
    if(rg)
    {
        UA_PubSubConnection *connection = rg->linkedConnection;
        if (connection->state == UA_PUBSUBSTATE_OPERATIONAL)
            ret = UA_ReaderGroup_setPubSubState(server, rg, UA_PUBSUBSTATE_PREOPERATIONAL,
                                            UA_STATUSCODE_GOOD);
        else if (connection->state == UA_PUBSUBSTATE_DISABLED || connection->state == UA_PUBSUBSTATE_PAUSED || connection->state == UA_PUBSUBSTATE_PREOPERATIONAL)
            ret = UA_ReaderGroup_setPubSubState(server, rg, UA_PUBSUBSTATE_PAUSED, UA_STATUSCODE_GOOD);
    }
    UA_UNLOCK(&server->serviceMutex);
    return ret;
}

UA_StatusCode
UA_Server_setReaderGroupDisabled(UA_Server *server, const UA_NodeId readerGroupId){
    UA_LOCK(&server->serviceMutex);
    UA_StatusCode ret = UA_STATUSCODE_BADNOTFOUND;
    UA_ReaderGroup *rg = UA_ReaderGroup_findRGbyId(server, readerGroupId);
    if(rg)
        ret = UA_ReaderGroup_setPubSubState(server, rg, UA_PUBSUBSTATE_DISABLED,
                                            UA_STATUSCODE_BADRESOURCEUNAVAILABLE);
    UA_UNLOCK(&server->serviceMutex);
    return ret;
}

#ifdef UA_ENABLE_PUBSUB_ENCRYPTION
UA_StatusCode
setReaderGroupEncryptionKeys(UA_Server *server, const UA_NodeId readerGroup,
                             UA_UInt32 securityTokenId,
                             const UA_ByteString signingKey,
                             const UA_ByteString encryptingKey,
                             const UA_ByteString keyNonce) {
    UA_ReaderGroup *rg = UA_ReaderGroup_findRGbyId(server, readerGroup);
    UA_CHECK_MEM(rg, return UA_STATUSCODE_BADNOTFOUND);
    if(rg->config.encodingMimeType == UA_PUBSUB_ENCODING_JSON) {
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg,
                                   "JSON encoding is enabled. The message security is "
                                   "only defined for the UADP message mapping.");
        return UA_STATUSCODE_BADINTERNALERROR;
    }
    if(!rg->config.securityPolicy) {
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg,
                                   "No SecurityPolicy configured for the ReaderGroup");
        return UA_STATUSCODE_BADINTERNALERROR;
    }

    if(securityTokenId != rg->securityTokenId) {
        rg->securityTokenId = securityTokenId;
        rg->nonceSequenceNumber = 1;
    }

    /* Create a new context */
    if(!rg->securityPolicyContext) {
        return rg->config.securityPolicy->
            newContext(rg->config.securityPolicy->policyContext,
                       &signingKey, &encryptingKey, &keyNonce,
                       &rg->securityPolicyContext);
    }

    /* Update the context */
    return rg->config.securityPolicy->
        setSecurityKeys(rg->securityPolicyContext, &signingKey,
                        &encryptingKey, &keyNonce);
}

UA_StatusCode
UA_Server_setReaderGroupEncryptionKeys(UA_Server *server,
                                       const UA_NodeId readerGroup,
                                       UA_UInt32 securityTokenId,
                                       const UA_ByteString signingKey,
                                       const UA_ByteString encryptingKey,
                                       const UA_ByteString keyNonce) {
    UA_LOCK(&server->serviceMutex);
    UA_StatusCode res = setReaderGroupEncryptionKeys(server, readerGroup,
                                                     securityTokenId, signingKey,
                                                     encryptingKey, keyNonce);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}
#endif

/* Freezing of the configuration */

UA_StatusCode
UA_ReaderGroup_freezeConfiguration(UA_Server *server, UA_ReaderGroup *rg) {
    UA_LOCK_ASSERT(&server->serviceMutex, 1);
    if(rg->configurationFrozen)
        return UA_STATUSCODE_GOOD;

    /* PubSubConnection freezeCounter++ */
    UA_PubSubConnection *pubSubConnection = rg->linkedConnection;
    pubSubConnection->configurationFreezeCounter++;

    /* ReaderGroup freeze */
    /* TODO: Clarify on the freeze functionality in multiple DSR, multiple
     * networkMessage conf in a RG */
    rg->configurationFrozen = true;

    /* DataSetReader freeze */
    UA_DataSetReader *dsr;
    UA_UInt16 dsrCount = 0;
    LIST_FOREACH(dsr, &rg->readers, listEntry){
        dsr->configurationFrozen = true;
        dsrCount++;
        /* TODO: Configuration frozen for subscribedDataSet once
         * UA_Server_DataSetReader_addTargetVariables API modified to support
         * adding target variable one by one or in a group stored in a list. */
    }

    /* Not rt, we don't have to adjust anything */
    if(rg->config.rtLevel != UA_PUBSUB_RT_FIXED_SIZE)
        return UA_STATUSCODE_GOOD;

    if(dsrCount > 1) {
        UA_LOG_WARNING_READERGROUP(&server->config.logger, rg,
                                   "Mutiple DSR in a readerGroup not supported in RT "
                                   "fixed size configuration");
        return UA_STATUSCODE_BADNOTIMPLEMENTED;
    }

    dsr = LIST_FIRST(&rg->readers);

    /* Support only to UADP encoding */
    if(dsr->config.messageSettings.content.decoded.type !=
       &UA_TYPES[UA_TYPES_UADPDATASETREADERMESSAGEDATATYPE]) {
        UA_LOG_WARNING_READER(&server->config.logger, dsr,
                              "PubSub-RT configuration fail: Non-RT capable encoding.");
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    /* Don't support string PublisherId for the fast-path (at this time) */
    if(!dsr->config.publisherId.type->pointerFree) {
        UA_LOG_WARNING_READER(&server->config.logger, dsr,
                              "PubSub-RT configuration fail: String PublisherId");
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }

    size_t fieldsSize = dsr->config.dataSetMetaData.fieldsSize;
    for(size_t i = 0; i < fieldsSize; i++) {
        UA_FieldTargetVariable *tv =
            &dsr->config.subscribedDataSet.subscribedDataSetTarget.targetVariables[i];
        const UA_VariableNode *rtNode = (const UA_VariableNode *)
            UA_NODESTORE_GET(server, &tv->targetVariable.targetNodeId);
        if(!rtNode ||
           rtNode->valueBackend.backendType != UA_VALUEBACKENDTYPE_EXTERNAL) {
            UA_LOG_WARNING_READER(&server->config.logger, dsr,
                                  "PubSub-RT configuration fail: PDS contains field "
                                  "without external data source.");
            UA_NODESTORE_RELEASE(server, (const UA_Node *) rtNode);
            return UA_STATUSCODE_BADNOTSUPPORTED;
        }

        /* Set the external data source in the tv */
        tv->externalDataValue = rtNode->valueBackend.backend.external.value;

        UA_NODESTORE_RELEASE(server, (const UA_Node *) rtNode);

        UA_FieldMetaData *field = &dsr->config.dataSetMetaData.fields[i];
        if((UA_NodeId_equal(&field->dataType, &UA_TYPES[UA_TYPES_STRING].typeId) ||
            UA_NodeId_equal(&field->dataType, &UA_TYPES[UA_TYPES_BYTESTRING].typeId)) &&
           field->maxStringLength == 0) {
            UA_LOG_WARNING_READER(&server->config.logger, dsr,
                                  "PubSub-RT configuration fail: "
                                  "PDS contains String/ByteString with dynamic length.");
            return UA_STATUSCODE_BADNOTSUPPORTED;
        } else if(!UA_DataType_isNumeric(UA_findDataType(&field->dataType)) &&
                  !UA_NodeId_equal(&field->dataType, &UA_TYPES[UA_TYPES_BOOLEAN].typeId)) {
            UA_LOG_WARNING_READER(&server->config.logger, dsr,
                                  "PubSub-RT configuration fail: "
                                  "PDS contains variable with dynamic size.");
            return UA_STATUSCODE_BADNOTSUPPORTED;
        }
    }

    /* Reset the OffsetBuffer. The OffsetBuffer for a frozen configuration is
     * generated when the first message is received. So we know the exact
     * settings which headers are present, etc. Until then the ReaderGroup is
     * "PreOperational". */
    UA_NetworkMessageOffsetBuffer_clear(&dsr->bufferedMessage);

    /* Set the current state again. This can move the state from Operational to
     * PreOperational. */
    return UA_ReaderGroup_setPubSubState(server, rg, rg->state, UA_STATUSCODE_GOOD);
}

UA_StatusCode
UA_Server_freezeReaderGroupConfiguration(UA_Server *server,
                                         const UA_NodeId readerGroupId) {
    UA_LOCK(&server->serviceMutex);
    UA_ReaderGroup *rg = UA_ReaderGroup_findRGbyId(server, readerGroupId);
    if(!rg) {
        UA_UNLOCK(&server->serviceMutex);
        return UA_STATUSCODE_BADNOTFOUND;
    }
    UA_StatusCode res = UA_ReaderGroup_freezeConfiguration(server, rg);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}

UA_StatusCode
UA_ReaderGroup_unfreezeConfiguration(UA_Server *server, UA_ReaderGroup *rg) {
    UA_LOCK_ASSERT(&server->serviceMutex, 1);

    /* PubSubConnection freezeCounter-- */
    UA_PubSubConnection *pubSubConnection = rg->linkedConnection;
    pubSubConnection->configurationFreezeCounter--;

    /* ReaderGroup unfreeze */
    rg->configurationFrozen = false;

    /* DataSetReader unfreeze */
    UA_DataSetReader *dataSetReader;
    LIST_FOREACH(dataSetReader, &rg->readers, listEntry) {
        dataSetReader->configurationFrozen = false;
        UA_NetworkMessageOffsetBuffer_clear(&dataSetReader->bufferedMessage);
    }

    return UA_STATUSCODE_GOOD;
}

UA_StatusCode
UA_Server_unfreezeReaderGroupConfiguration(UA_Server *server,
                                           const UA_NodeId readerGroupId) {
    UA_LOCK(&server->serviceMutex);
    UA_ReaderGroup *rg = UA_ReaderGroup_findRGbyId(server, readerGroupId);
    if(!rg) {
        UA_UNLOCK(&server->serviceMutex);
        return UA_STATUSCODE_BADNOTFOUND;
    }
    UA_StatusCode res = UA_ReaderGroup_unfreezeConfiguration(server, rg);
    UA_UNLOCK(&server->serviceMutex);
    return res;
}

#endif /* UA_ENABLE_PUBSUB */
