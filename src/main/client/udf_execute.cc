/*******************************************************************************
 * Copyright 2013-2014 Aerospike, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

extern "C" {
    #include <aerospike/aerospike.h>
    #include <aerospike/aerospike_key.h>
    #include <aerospike/as_config.h>
    #include <aerospike/as_key.h>
    #include <aerospike/as_record.h>
    #include <aerospike/as_record_iterator.h>
}

#include <node.h>
#include <cstdlib>
#include <unistd.h>

#include "../client.h"
#include "../util/async.h"
#include "../util/conversions.h"
#include "../util/log.h"


#define FILESIZE		 255
#define UDF_ARG_KEY      0 
#define UDF_ARG_UDFARGS  1
#define UDF_ARG_APOLICY  2 // apply policy position and callback position is not same 
#define UDF_ARG_CB       3 // for every invoke of udf_execute. If applypolicy is not passed from node
						   // application, argument position for callback changes.

using namespace v8;

/*******************************************************************************
 *  TYPES
 ******************************************************************************/

/**
 *  AsyncData — Data to be used in async calls.
 *
 *  libuv allows us to pass around a pointer to an arbitraty object when
 *  running asynchronous functions. We create a data structure to hold the 
 *  data we need during and after async work.
 */

typedef struct AsyncData {
    aerospike * as;
    int param_err;
    as_error err;
    as_policy_apply policy;
    as_key key;
	char filename[FILESIZE];
	char funcname[FILESIZE];
	as_arraylist udfargs;
    LogInfo * log;
	as_val* result;
    Persistent<Function> callback;
} AsyncData;


/*******************************************************************************
 *  FUNCTIONS
 ******************************************************************************/

/**
 *  prepare() — Function to prepare AsyncData, for use in `execute()` and `respond()`.
 *  
 *  This should only keep references to V8 or V8 structures for use in 
 *  `respond()`, because it is unsafe for use in `execute()`.
 */
static void * prepare(const Arguments& args)
{
    // The current scope of the function
    NODE_ISOLATE_DECL;
    HANDLESCOPE;

    // Unwrap 'this'
    AerospikeClient * client    = ObjectWrap::Unwrap<AerospikeClient>(args.This());

    // Build the async data
    AsyncData * data            = new AsyncData;
    data->as                    = client->as;

	// Initialize default values in async data
    data->param_err             = 0;
	data->result				= NULL;

    // Local variables
    as_key *    key             = &data->key;
    as_policy_apply* policy     = &data->policy;
    LogInfo * log               = data->log = client->log;
	as_arraylist* udfargs		= &data->udfargs;
	char * filename				= data->filename;  
	char * funcname				= data->funcname;
    int arglength				= args.Length();

	memset(data->filename, 0, FILESIZE);
	memset(data->filename, 0, FILESIZE);

    if ( args[arglength-1]->IsFunction()) {
        data->callback = Persistent<Function>::New(NODE_ISOLATE_PRE Local<Function>::Cast(args[arglength-1]));
        as_v8_detail(log, "Node.js Callback Registered");
    }
    else {
        as_v8_error(log, "No callback to register");
        COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
        goto Err_Return;
    }

    if ( args[UDF_ARG_KEY]->IsObject()) {
        if (key_from_jsobject(key, args[UDF_ARG_KEY]->ToObject(), log) != AS_NODE_PARAM_OK ) {
            as_v8_error(log,"Parsing as_key(C structure) from key object failed");
            COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
            goto Err_Return;
        }
    }
    else {
        as_v8_error(log, "Key should be an object");
        COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
        goto Err_Return;
    }
   
	// filename length should be finalized.
    if ( args[UDF_ARG_UDFARGS]->IsObject() ) {	
		if ( udfargs_from_jsobject( &filename, &funcname, &udfargs, args[UDF_ARG_UDFARGS]->ToObject(), log) != AS_NODE_PARAM_OK ) {
			as_v8_error(log, "Parsing UDF arguments failed"); 
			COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
			goto Err_Return;
		}
    }
    else {
        as_v8_error(log, "UDF args should be an object"); 
        COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
        goto Err_Return;
    }
    if ( arglength > 3 ) {
        if ( args[UDF_ARG_APOLICY]->IsObject() &&
                applypolicy_from_jsobject(policy, args[UDF_ARG_APOLICY]->ToObject(), log) != AS_NODE_PARAM_OK) {
            as_v8_error(log, "apply policy shoule be an object");
            COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
            goto Err_Return;
        } 
    }
    else {
        // When node application does not pass any apply policy should be 
        // initialized to defaults,
        as_v8_debug(log, "Argument list does not contain applypolicy, default applypolicy will be used");
        as_policy_apply_init(policy);
    }

    as_v8_debug(log, "Parsing node.js Data Structures : Success");
    scope.Close(Undefined());
    return data;

Err_Return:
    data->param_err = 1;
    scope.Close(Undefined());
    return data;
}

/**
 *  execute() — Function to execute inside the worker-thread.
 *  
 *  It is not safe to access V8 or V8 data structures here, so everything
 *  we need for input and output should be in the AsyncData structure.
 */
static void execute(uv_work_t * req)
{
    // Fetch the AsyncData structure
    AsyncData * data         = reinterpret_cast<AsyncData *>(req->data);
    aerospike * as           = data->as;
    as_error *  err          = &data->err;
    as_key *    key          = &data->key;
    as_policy_apply * policy = &data->policy;
    LogInfo * log            = data->log;

    // Invoke the blocking call.
    // The error is handled in the calling JS code.
    if (as->cluster == NULL) {
        data->param_err = 1;
        COPY_ERR_MESSAGE(data->err, AEROSPIKE_ERR_PARAM);
        as_v8_error(log, "Not connected to cluster to execute record udf");
    }

    if ( data->param_err == 0) {
        as_v8_debug(log, "Invoking aerospike execute ");
        aerospike_key_apply(as, err, policy, key, data->filename, data->funcname, (as_list*) &data->udfargs, &data->result);
    }

}

/**
 *  AfterWork — Function to execute when the Work is complete
 *
 *  This function will be run inside the main event loop so it is safe to use 
 *  V8 again. This is where you will convert the results into V8 types, and 
 *  call the callback function with those results.
 */
static void respond(uv_work_t * req, int status)
{
    // Scope for the callback operation.
    NODE_ISOLATE_DECL;
    HANDLESCOPE;

    // Fetch the AsyncData structure
    AsyncData * data    = reinterpret_cast<AsyncData *>(req->data);
    as_error *  err     = &data->err;
    as_key *    key     = &data->key;
    LogInfo * log       = data->log;
    as_v8_debug(log, "UDF execute operation : response is %d", err->code);

    // Build the arguments array for the callback
    Handle<Value> argv[3];
    if (data->param_err == 0) {
        argv[0] = error_to_jsobject(err, log);

		// should it be moved into a new function.
		// APIs for query and scan should determine that.
		Handle<Object> res  = Object::New();
		Handle<Value> jsval = val_to_jsvalue( data->result, log);
		res->Set( String::NewSymbol( data->funcname), jsval);
		argv[1] = res;
        argv[2] = key_to_jsobject(key, log);
    }
    else {
        err->func = NULL;
        as_v8_debug(log, "Parameter error for udf execute operation");
        argv[0] = error_to_jsobject(err, log);
        argv[1] = Null();
		argv[2] = Null();
    }   

    // Surround the callback in a try/catch for safety
    TryCatch try_catch;

    // Execute the callback.
    if ( data->callback != Null() ) {
        data->callback->Call(Context::GetCurrent()->Global(), 3, argv);
        as_v8_debug(log, "Invoked udf execute callback");
    }

    // Process the exception, if any
    if ( try_catch.HasCaught() ) {
        node::FatalException(try_catch);
    }

    // Dispose the Persistent handle so the callback
    // function can be garbage-collected
    data->callback.Dispose();

    // clean up any memory we allocated

    if ( data->param_err == 0) {
        as_key_destroy(key);
        as_v8_debug(log, "Cleaned up key structure");
    }

    delete data;
    delete req;
    scope.Close(Undefined());
}

/*******************************************************************************
 *  OPERATION
 ******************************************************************************/

/**
 *  The 'put()' Operation
 */
Handle<Value> AerospikeClient::Execute(const Arguments& args)
{
    return async_invoke(args, prepare, execute, respond);
}
