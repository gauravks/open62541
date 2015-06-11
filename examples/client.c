#ifdef NOT_AMALGATED
    #include "ua_types.h"
    #include "ua_client.h"
    #include "ua_nodeids.h"
    #include "networklayer_tcp.h"
    #include "logger_stdout.h"
#else
    #include "open62541.h"
#endif

#include <stdio.h>

void handler_TheAnswerChanged(UA_UInt32 handle, UA_DataValue *value);
void handler_TheAnswerChanged(UA_UInt32 handle, UA_DataValue *value) {
    printf("Handler called");
    return;
}

int main(int argc, char *argv[]) {
    UA_Client *client = UA_Client_new(UA_ClientConfig_standard, Logger_Stdout_new());
    UA_StatusCode retval = UA_Client_connect(client, ClientNetworkLayerTCP_connect,
                                             "opc.tcp://localhost:16664");
    if(retval != UA_STATUSCODE_GOOD) {
        UA_Client_delete(client);
        return retval;
    }

    // Browse some objects
    printf("Browsing nodes in objects folder:\n");

    UA_BrowseRequest bReq;
    UA_BrowseRequest_init(&bReq);
    bReq.requestedMaxReferencesPerNode = 0;
    bReq.nodesToBrowse = UA_BrowseDescription_new();
    bReq.nodesToBrowseSize = 1;
    bReq.nodesToBrowse[0].nodeId = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER); //browse objects folder
    bReq.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_ALL; //return everything

    UA_BrowseResponse bResp = UA_Client_browse(client, &bReq);
    printf("%-9s %-16s %-16s %-16s\n", "NAMESPACE", "NODEID", "BROWSE NAME", "DISPLAY NAME");
    for (int i = 0; i < bResp.resultsSize; ++i) {
        for (int j = 0; j < bResp.results[i].referencesSize; ++j) {
            UA_ReferenceDescription *ref = &(bResp.results[i].references[j]);
            if(ref->nodeId.nodeId.identifierType == UA_NODEIDTYPE_NUMERIC) {
                printf("%-9d %-16d %-16.*s %-16.*s\n", ref->browseName.namespaceIndex,
                       ref->nodeId.nodeId.identifier.numeric, ref->browseName.name.length,
                       ref->browseName.name.data, ref->displayName.text.length, ref->displayName.text.data);
            } else if(ref->nodeId.nodeId.identifierType == UA_NODEIDTYPE_STRING) {
                printf("%-9d %-16.*s %-16.*s %-16.*s\n", ref->browseName.namespaceIndex,
                       ref->nodeId.nodeId.identifier.string.length, ref->nodeId.nodeId.identifier.string.data,
                       ref->browseName.name.length, ref->browseName.name.data, ref->displayName.text.length,
                       ref->displayName.text.data);
            }
            //TODO: distinguish further types
        }
    }
    UA_BrowseRequest_deleteMembers(&bReq);
    UA_BrowseResponse_deleteMembers(&bResp);

    UA_Int32 value = 0;
    // Read node's value
    printf("\nReading the value of node (1, \"the.answer\"):\n");
    UA_ReadRequest rReq;
    UA_ReadRequest_init(&rReq);
    rReq.nodesToRead = UA_ReadValueId_new();
    rReq.nodesToReadSize = 1;
    rReq.nodesToRead[0].nodeId = UA_NODEID_STRING_ALLOC(1, "the.answer"); /* assume this node exists */
    rReq.nodesToRead[0].attributeId = UA_ATTRIBUTEID_VALUE;

    UA_ReadResponse rResp = UA_Client_read(client, &rReq);
    if(rResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD &&
       rResp.resultsSize > 0 && rResp.results[0].hasValue &&
       UA_Variant_isScalar(&rResp.results[0].value) &&
       rResp.results[0].value.type == &UA_TYPES[UA_TYPES_INT32]) {
        value = *(UA_Int32*)rResp.results[0].value.data;
        printf("the value is: %i\n", value);
    }

    UA_ReadRequest_deleteMembers(&rReq);
    UA_ReadResponse_deleteMembers(&rResp);

    value++;
    // Write node's value
    printf("\nWriting a value of node (1, \"the.answer\"):\n");
    UA_WriteRequest wReq;
    UA_WriteRequest_init(&wReq);
    wReq.nodesToWrite = UA_WriteValue_new();
    wReq.nodesToWriteSize = 1;
    wReq.nodesToWrite[0].nodeId = UA_NODEID_STRING_ALLOC(1, "the.answer"); /* assume this node exists */
    wReq.nodesToWrite[0].attributeId = UA_ATTRIBUTEID_VALUE;
    wReq.nodesToWrite[0].value.hasValue = UA_TRUE;
    wReq.nodesToWrite[0].value.value.type = &UA_TYPES[UA_TYPES_INT32];
    wReq.nodesToWrite[0].value.value.storageType = UA_VARIANT_DATA_NODELETE; //do not free the integer on deletion
    wReq.nodesToWrite[0].value.value.data = &value;

    UA_WriteResponse wResp = UA_Client_write(client, &wReq);
    if(wResp.responseHeader.serviceResult == UA_STATUSCODE_GOOD)
            printf("the new value is: %i\n", value);

    UA_WriteRequest_deleteMembers(&wReq);
    UA_WriteResponse_deleteMembers(&wResp);
    
    // Create a subscription
    UA_Int32 subId = UA_Client_newSubscription(client);
    if (subId)
        printf("Create subscription succeeded, id %u\n", subId);
    
    // Monitor TheAnswer
    UA_NodeId monitorThis;
    monitorThis = UA_NODEID_STRING_ALLOC(1, "the.answer");
    UA_UInt32 monId = UA_Client_monitorItemChanges(client, subId, monitorThis, UA_ATTRIBUTEID_VALUE, &handler_TheAnswerChanged );
    if (monId)
        printf("Monitoring 'the.answer', id %u\n", subId);
    UA_NodeId_deleteMembers(&monitorThis);
    UA_Client_unMonitorItemChanges(client, subId, monId);
    
    if(!UA_Client_removeSubscription(client, subId))
        printf("Subscription removed\n");
    
    UA_Client_disconnect(client);
    UA_Client_delete(client);
    return UA_STATUSCODE_GOOD;
}

