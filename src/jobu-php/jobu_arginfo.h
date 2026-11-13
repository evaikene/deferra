/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 1c135b8509e1c94a11ef37e2fab0b3181575439b */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_Duration___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Duration_getMilliseconds, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Duration_milliseconds, 0, 1, JobU\\Duration, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Duration_seconds arginfo_class_JobU_Duration_milliseconds

#define arginfo_class_JobU_Duration_minutes arginfo_class_JobU_Duration_milliseconds

#define arginfo_class_JobU_Duration_hours arginfo_class_JobU_Duration_milliseconds

#define arginfo_class_JobU_Duration_days arginfo_class_JobU_Duration_milliseconds

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_SecretReference___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_SecretReference_getName, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_CliPayload___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, command, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arguments, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_setCommand, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, command, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, arguments, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CliPayload_getExecutable arginfo_class_JobU_SecretReference_getName

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_setExecutable, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, executable, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_getArguments, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_setArguments, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, arguments, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_addArgument, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, argument, JobU\\SecretReference, MAY_BE_STRING, NULL)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CliPayload_getEnvironment arginfo_class_JobU_CliPayload_getArguments

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_setEnvironment, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, value, JobU\\SecretReference, MAY_BE_STRING, NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_unsetEnvironment, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CliPayload_clearEnvironment arginfo_class_JobU_CliPayload_unsetEnvironment

#define arginfo_class_JobU_CliPayload_getWorkingDirectory arginfo_class_JobU_SecretReference_getName

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_setWorkingDirectory, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, directory, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CliPayload_getExpectedExitCodes arginfo_class_JobU_CliPayload_getArguments

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliPayload_setExpectedExitCodes, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, codes, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_HttpPayload___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, url, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, method, IS_STRING, 0, "\'GET\'")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_HttpPayload_getUrl arginfo_class_JobU_SecretReference_getName

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_setUrl, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, url, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_HttpPayload_getMethod arginfo_class_JobU_SecretReference_getName

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_setMethod, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, method, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_addHeader, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, value, JobU\\SecretReference, MAY_BE_STRING, NULL)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, sensitive, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_HttpPayload_getHeaders arginfo_class_JobU_CliPayload_getArguments

#define arginfo_class_JobU_HttpPayload_clearHeader arginfo_class_JobU_CliPayload_unsetEnvironment

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_setBody, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, body, JobU\\SecretReference, MAY_BE_STRING, NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_class_JobU_HttpPayload_getBody, 0, 0, JobU\\SecretReference, MAY_BE_STRING|MAY_BE_NULL)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_clearBody, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_setJsonBody, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_HttpPayload_getExpectedStatuses arginfo_class_JobU_CliPayload_getArguments

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpPayload_setExpectedStatuses, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, statuses, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_HttpHeader___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_HttpHeader_getName arginfo_class_JobU_SecretReference_getName

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_class_JobU_HttpHeader_getValue, 0, 0, JobU\\SecretReference, MAY_BE_STRING)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpHeader_isSensitive, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_OnceSchedule___construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, at, DateTimeInterface, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_OnceSchedule_now, 0, 0, JobU\\OnceSchedule, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_OnceSchedule_isNow arginfo_class_JobU_HttpHeader_isSensitive

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_OnceSchedule_getAt, 0, 0, DateTimeImmutable, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_CronSchedule___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, expression, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timezone, DateTimeZone, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CronSchedule_getExpression arginfo_class_JobU_SecretReference_getName

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_CronSchedule_getTimezone, 0, 0, DateTimeZone, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_JobAttributes___construct arginfo_class_JobU_Duration___construct

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_get, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_has, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_set, 0, 2, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_JobAttributes_getTimeout, 0, 0, JobU\\Duration, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_setTimeout, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, value, JobU\\Duration, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_getOutputCapture, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_setOutputCapture, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_JobAttributes_getRetryInitialDelay arginfo_class_JobU_JobAttributes_getTimeout

#define arginfo_class_JobU_JobAttributes_setRetryInitialDelay arginfo_class_JobU_JobAttributes_setTimeout

#define arginfo_class_JobU_JobAttributes_getRetryMaxDelay arginfo_class_JobU_JobAttributes_getTimeout

#define arginfo_class_JobU_JobAttributes_setRetryMaxDelay arginfo_class_JobU_JobAttributes_setTimeout

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_getRetryMaxAttempts, 0, 0, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_setRetryMaxAttempts, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_getRetryJitter, 0, 0, IS_DOUBLE, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_setRetryJitter, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_DOUBLE, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_JobAttributes_getRetryMultiplier arginfo_class_JobU_JobAttributes_getRetryJitter

#define arginfo_class_JobU_JobAttributes_setRetryMultiplier arginfo_class_JobU_JobAttributes_setRetryJitter

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_JobAttributes_getRetryMode, 0, 0, JobU\\RetryMode, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_JobAttributes_setRetryMode, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, value, JobU\\RetryMode, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_JobAttributes_getRetryStrategy arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_JobAttributes_setRetryStrategy arginfo_class_JobU_JobAttributes_setOutputCapture

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliJobAttributes_getRetryExitCodes, 0, 0, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_CliJobAttributes_setRetryExitCodes, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CliJobAttributes_getTerminationGrace arginfo_class_JobU_JobAttributes_getTimeout

#define arginfo_class_JobU_CliJobAttributes_setTerminationGrace arginfo_class_JobU_JobAttributes_setTimeout

#define arginfo_class_JobU_CliJobAttributes_getStdoutLimit arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_CliJobAttributes_setStdoutLimit arginfo_class_JobU_JobAttributes_setRetryMaxAttempts

#define arginfo_class_JobU_CliJobAttributes_getStderrLimit arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_CliJobAttributes_setStderrLimit arginfo_class_JobU_JobAttributes_setRetryMaxAttempts

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpJobAttributes_getFollowRedirects, 0, 0, _IS_BOOL, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_HttpJobAttributes_setFollowRedirects, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, value, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_HttpJobAttributes_getIdempotencyKey arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

#define arginfo_class_JobU_HttpJobAttributes_setIdempotencyKey arginfo_class_JobU_HttpJobAttributes_setFollowRedirects

#define arginfo_class_JobU_HttpJobAttributes_getTlsVerify arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

#define arginfo_class_JobU_HttpJobAttributes_setTlsVerify arginfo_class_JobU_HttpJobAttributes_setFollowRedirects

#define arginfo_class_JobU_HttpJobAttributes_getMaxRedirects arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_HttpJobAttributes_setMaxRedirects arginfo_class_JobU_JobAttributes_setRetryMaxAttempts

#define arginfo_class_JobU_HttpJobAttributes_getHttpBodyLimit arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_HttpJobAttributes_setHttpBodyLimit arginfo_class_JobU_JobAttributes_setRetryMaxAttempts

#define arginfo_class_JobU_HttpJobAttributes_getHttpHeadersLimit arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_HttpJobAttributes_setHttpHeadersLimit arginfo_class_JobU_JobAttributes_setRetryMaxAttempts

#define arginfo_class_JobU_HttpJobAttributes_getRetryErrors arginfo_class_JobU_CliJobAttributes_getRetryExitCodes

#define arginfo_class_JobU_HttpJobAttributes_setRetryErrors arginfo_class_JobU_CliJobAttributes_setRetryExitCodes

#define arginfo_class_JobU_HttpJobAttributes_getRetryStatuses arginfo_class_JobU_CliJobAttributes_getRetryExitCodes

#define arginfo_class_JobU_HttpJobAttributes_setRetryStatuses arginfo_class_JobU_CliJobAttributes_setRetryExitCodes

#define arginfo_class_JobU_QueueDefaults___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_QueueDefaults_get arginfo_class_JobU_JobAttributes_get

#define arginfo_class_JobU_QueueDefaults_has arginfo_class_JobU_JobAttributes_has

#define arginfo_class_JobU_QueueDefaults_set arginfo_class_JobU_JobAttributes_set

#define arginfo_class_JobU_QueueDefaults_clear arginfo_class_JobU_CliPayload_unsetEnvironment

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_QueueDefaults_cli, 0, 0, JobU\\CliJobAttributes, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_QueueDefaults_http, 0, 0, JobU\\HttpJobAttributes, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_Client___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, socketPath, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Client_close arginfo_class_JobU_HttpPayload_clearBody

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Client_listQueues, 0, 0, JobU\\Page, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, state, JobU\\QueueState, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeDeleted, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "100")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Client_listJobs, 0, 0, JobU\\Page, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, queueName, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, queueId, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, state, JobU\\JobState, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, type, JobU\\JobType, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeDeleted, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "100")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Client_getJob, 0, 1, JobU\\Job, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Client_getQueue, 0, 1, JobU\\Queue, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeDeleted, _IS_BOOL, 0, "false")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Client_getQueueById, 0, 1, JobU\\Queue, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeDeleted, _IS_BOOL, 0, "false")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Client_createQueue, 0, 1, JobU\\Queue, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, weight, IS_LONG, 0, "1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, concurrencyLimit, IS_LONG, 0, "1")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, recoveryPolicy, JobU\\RecoveryPolicy, 0, "JobU\\RecoveryPolicy::FailInterrupted")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, defaults, JobU\\QueueDefaults, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, historyRetention, JobU\\Duration, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, runnableWaitWarning, JobU\\Duration, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, idempotencyKey, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_listJobs, 0, 0, JobU\\Page, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, state, JobU\\JobState, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, type, JobU\\JobType, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeDeleted, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "100")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_createCliJob, 0, 2, JobU\\Job, 0)
	ZEND_ARG_OBJ_INFO(0, payload, JobU\\CliPayload, 0)
	ZEND_ARG_OBJ_INFO(0, schedule, JobU\\Schedule, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, name, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, priority, IS_LONG, 0, "0")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, attributes, JobU\\JobAttributes, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, idempotencyKey, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_createHttpJob, 0, 2, JobU\\Job, 0)
	ZEND_ARG_OBJ_INFO(0, payload, JobU\\HttpPayload, 0)
	ZEND_ARG_OBJ_INFO(0, schedule, JobU\\Schedule, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, name, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, priority, IS_LONG, 0, "0")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, attributes, JobU\\JobAttributes, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, idempotencyKey, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_Queue_getId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Queue_getRevision arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_Queue_getName arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Queue_setName arginfo_class_JobU_CliPayload_unsetEnvironment

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_getState, 0, 0, JobU\\QueueState, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue_getWeight arginfo_class_JobU_Duration_getMilliseconds

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_setWeight, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, weight, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue_getConcurrencyLimit arginfo_class_JobU_Duration_getMilliseconds

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_setConcurrencyLimit, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, concurrencyLimit, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_getRecoveryPolicy, 0, 0, JobU\\RecoveryPolicy, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_setRecoveryPolicy, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, recoveryPolicy, JobU\\RecoveryPolicy, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue_getHistoryRetention arginfo_class_JobU_JobAttributes_getTimeout

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_setHistoryRetention, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, historyRetention, JobU\\Duration, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_getRunnableWaitWarning, 0, 0, JobU\\Duration, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_setRunnableWaitWarning, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, runnableWaitWarning, JobU\\Duration, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_getCreatedAt, 0, 0, DateTimeImmutable, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue_getUpdatedAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_Queue_getDeletedAt arginfo_class_JobU_OnceSchedule_getAt

#define arginfo_class_JobU_Queue_isDirty arginfo_class_JobU_HttpHeader_isSensitive

#define arginfo_class_JobU_Queue_isDeleted arginfo_class_JobU_HttpHeader_isSensitive

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Queue_getDefaults, 0, 0, JobU\\QueueDefaults, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue_defaults arginfo_class_JobU_Queue_getDefaults

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_setDefaults, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, defaults, JobU\\QueueDefaults, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_save, 0, 0, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Queue_refresh, 0, 0, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, includeDeleted, _IS_BOOL, 0, "false")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Queue_discardChanges arginfo_class_JobU_HttpPayload_clearBody

#define arginfo_class_JobU_Queue_suspend arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Queue_resume arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Queue_delete arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Exception_getErrorCode arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_Exception_getCategory arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_OutcomeUnknownException_getOperation arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_OutcomeUnknownException_getIdempotencyKey arginfo_class_JobU_JobAttributes_getOutputCapture

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_TimeRange___construct, 0, 0, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, from, DateTimeInterface, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, to, DateTimeInterface, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_TimeRange_getFrom arginfo_class_JobU_OnceSchedule_getAt

#define arginfo_class_JobU_TimeRange_getTo arginfo_class_JobU_OnceSchedule_getAt

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_RunQuery___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, queueId, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, jobId, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, state, JobU\\RunState, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, states, IS_ARRAY, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, origin, JobU\\RunOrigin, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, type, JobU\\JobType, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, planned, JobU\\TimeRange, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, started, JobU\\TimeRange, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, completed, JobU\\TimeRange, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "100")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_RunQuery_getQueueId arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_RunQuery_getJobId arginfo_class_JobU_JobAttributes_getOutputCapture

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_RunQuery_getState, 0, 0, JobU\\RunState, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_RunQuery_getStates arginfo_class_JobU_CliJobAttributes_getRetryExitCodes

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_RunQuery_getOrigin, 0, 0, JobU\\RunOrigin, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_RunQuery_getType, 0, 0, JobU\\JobType, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_RunQuery_getPlanned, 0, 0, JobU\\TimeRange, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_RunQuery_getStarted arginfo_class_JobU_RunQuery_getPlanned

#define arginfo_class_JobU_RunQuery_getCompleted arginfo_class_JobU_RunQuery_getPlanned

#define arginfo_class_JobU_RunQuery_getLimit arginfo_class_JobU_Duration_getMilliseconds

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_StatisticsQuery___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, queueId, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, jobId, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, type, JobU\\JobType, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, origin, JobU\\RunOrigin, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, planned, JobU\\TimeRange, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, groupBy, JobU\\StatisticsGroupBy, 0, "JobU\\StatisticsGroupBy::None")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "100")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_StatisticsQuery_getQueueId arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_StatisticsQuery_getJobId arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_StatisticsQuery_getType arginfo_class_JobU_RunQuery_getType

#define arginfo_class_JobU_StatisticsQuery_getOrigin arginfo_class_JobU_RunQuery_getOrigin

#define arginfo_class_JobU_StatisticsQuery_getPlanned arginfo_class_JobU_RunQuery_getPlanned

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_StatisticsQuery_getGroupBy, 0, 0, JobU\\StatisticsGroupBy, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_StatisticsQuery_getLimit arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_SystemInfo___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_SystemInfo_getDaemonVersion arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_SystemInfo_getApiMajor arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_SystemInfo_getApiMinor arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_SystemInfo_getCapabilities arginfo_class_JobU_CliPayload_getArguments

#define arginfo_class_JobU_SystemInfo_hasCapability arginfo_class_JobU_JobAttributes_has

#define arginfo_class_JobU_SecretMetadata___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_SecretMetadata_getName arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_SecretMetadata_getCreatedAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_SecretMetadata_getUpdatedAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_ExecutionResult___construct arginfo_class_JobU_Duration___construct

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_ExecutionResult_getType, 0, 0, JobU\\JobType, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_ExecutionResult_getOutcome arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_ExecutionResult_getReason arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_ExecutionResult_getErrorCode arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_ExecutionResult_getOutcomeUnknown arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_ExecutionResult_has, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, field, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_ExecutionResult_get, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, field, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_ExecutionResult_getCapturedBytes, 0, 1, IS_LONG, 1)
	ZEND_ARG_OBJ_INFO(0, channel, JobU\\OutputChannel, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_ExecutionResult_getTotalBytes arginfo_class_JobU_ExecutionResult_getCapturedBytes

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_ExecutionResult_isTruncated, 0, 1, _IS_BOOL, 1)
	ZEND_ARG_OBJ_INFO(0, channel, JobU\\OutputChannel, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_CliExecutionResult_getExitCode arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_CliExecutionResult_getSignal arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_CliExecutionResult_getCaptureLost arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

#define arginfo_class_JobU_HttpExecutionResult_getStatus arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_HttpExecutionResult_getDuration arginfo_class_JobU_JobAttributes_getTimeout

#define arginfo_class_JobU_HttpExecutionResult_getRedirects arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_HttpExecutionResult_getTlsVerified arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

#define arginfo_class_JobU_HttpExecutionResult_getErrorCategory arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_HttpExecutionResult_getRetryAfterAccepted arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

#define arginfo_class_JobU_HttpExecutionResult_getRetryAfterClamped arginfo_class_JobU_HttpJobAttributes_getFollowRedirects

#define arginfo_class_JobU_OutputChunk___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_OutputChunk_getRunId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_OutputChunk_getAttemptNumber arginfo_class_JobU_Duration_getMilliseconds

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_OutputChunk_getChannel, 0, 0, JobU\\OutputChannel, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_OutputChunk_getStatus, 0, 0, JobU\\OutputStatus, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_OutputChunk_getOffset arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_OutputChunk_getBytesReturned arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_OutputChunk_getNextOffset arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_OutputChunk_getRetainedBytes arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_OutputChunk_getTotalBytes arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_OutputChunk_getOmittedBytes arginfo_class_JobU_JobAttributes_getRetryMaxAttempts

#define arginfo_class_JobU_OutputChunk_isTruncated arginfo_class_JobU_HttpHeader_isSensitive

#define arginfo_class_JobU_OutputChunk_isCaptureLost arginfo_class_JobU_HttpHeader_isSensitive

#define arginfo_class_JobU_OutputChunk_getData arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Job___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_Job_getId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Job_getQueueId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Job_getRevision arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_Job_getName arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_Job_getPriority arginfo_class_JobU_Duration_getMilliseconds

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Job_getState, 0, 0, JobU\\JobState, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Job_getType arginfo_class_JobU_ExecutionResult_getType

#define arginfo_class_JobU_Job_getCreatedAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_Job_getUpdatedAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_Job_getDeletedAt arginfo_class_JobU_OnceSchedule_getAt

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Job_getSchedule, 0, 0, JobU\\Schedule, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Job_getAttributes, 0, 0, JobU\\JobAttributes, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Job_attributes arginfo_class_JobU_Job_getAttributes

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_TYPE_MASK_EX(arginfo_class_JobU_Job_getPayload, 0, 0, JobU\\CliPayload|JobU\\HttpPayload, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Job_payload arginfo_class_JobU_Job_getPayload

#define arginfo_class_JobU_Job_isDirty arginfo_class_JobU_HttpHeader_isSensitive

#define arginfo_class_JobU_Job_isDeleted arginfo_class_JobU_HttpHeader_isSensitive

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Job_setName, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Job_setPriority, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, priority, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Job_setSchedule, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, schedule, JobU\\Schedule, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Job_setAttributes, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, attributes, JobU\\JobAttributes, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Job_setPayload, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_TYPE_MASK(0, payload, JobU\\CliPayload|JobU\\HttpPayload, 0, NULL)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Job_save arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Job_refresh arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Job_discardChanges arginfo_class_JobU_HttpPayload_clearBody

#define arginfo_class_JobU_Job_suspend arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Job_resume arginfo_class_JobU_Queue_save

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Job_move, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, queue, JobU\\Queue, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Job_delete arginfo_class_JobU_Queue_save

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Job_runNow, 0, 0, JobU\\Run, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, idempotencyKey, IS_STRING, 1, "null")
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Run___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_Run_getId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Run_getJobId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Run_getQueueId arginfo_class_JobU_SecretReference_getName

#define arginfo_class_JobU_Run_getJobRevision arginfo_class_JobU_Duration_getMilliseconds

#define arginfo_class_JobU_Run_getPriority arginfo_class_JobU_Duration_getMilliseconds

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Run_getOrigin, 0, 0, JobU\\RunOrigin, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Run_getType arginfo_class_JobU_ExecutionResult_getType

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Run_getState, 0, 0, JobU\\RunState, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Run_isScheduleOwned arginfo_class_JobU_HttpHeader_isSensitive

#define arginfo_class_JobU_Run_getPlannedAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_Run_getRunnableAt arginfo_class_JobU_Queue_getCreatedAt

#define arginfo_class_JobU_Run_getStartedAt arginfo_class_JobU_OnceSchedule_getAt

#define arginfo_class_JobU_Run_getCompletedAt arginfo_class_JobU_OnceSchedule_getAt

#define arginfo_class_JobU_Run_getAttributes arginfo_class_JobU_Job_getAttributes

#define arginfo_class_JobU_Run_getPayload arginfo_class_JobU_Job_getPayload

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Run_getResult, 0, 0, JobU\\ExecutionResult, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Page___construct arginfo_class_JobU_Duration___construct

#define arginfo_class_JobU_Page_hasNext arginfo_class_JobU_HttpHeader_isSensitive

#define arginfo_class_JobU_Page_next arginfo_class_JobU_Queue_save

#define arginfo_class_JobU_Page_items arginfo_class_JobU_CliPayload_getArguments

ZEND_METHOD(JobU_Duration, __construct);
ZEND_METHOD(JobU_Duration, getMilliseconds);
ZEND_METHOD(JobU_Duration, milliseconds);
ZEND_METHOD(JobU_Duration, seconds);
ZEND_METHOD(JobU_Duration, minutes);
ZEND_METHOD(JobU_Duration, hours);
ZEND_METHOD(JobU_Duration, days);
ZEND_METHOD(JobU_SecretReference, __construct);
ZEND_METHOD(JobU_SecretReference, getName);
ZEND_METHOD(JobU_CliPayload, __construct);
ZEND_METHOD(JobU_CliPayload, setCommand);
ZEND_METHOD(JobU_CliPayload, getExecutable);
ZEND_METHOD(JobU_CliPayload, setExecutable);
ZEND_METHOD(JobU_CliPayload, getArguments);
ZEND_METHOD(JobU_CliPayload, setArguments);
ZEND_METHOD(JobU_CliPayload, addArgument);
ZEND_METHOD(JobU_CliPayload, getEnvironment);
ZEND_METHOD(JobU_CliPayload, setEnvironment);
ZEND_METHOD(JobU_CliPayload, unsetEnvironment);
ZEND_METHOD(JobU_CliPayload, clearEnvironment);
ZEND_METHOD(JobU_CliPayload, getWorkingDirectory);
ZEND_METHOD(JobU_CliPayload, setWorkingDirectory);
ZEND_METHOD(JobU_CliPayload, getExpectedExitCodes);
ZEND_METHOD(JobU_CliPayload, setExpectedExitCodes);
ZEND_METHOD(JobU_HttpPayload, __construct);
ZEND_METHOD(JobU_HttpPayload, getUrl);
ZEND_METHOD(JobU_HttpPayload, setUrl);
ZEND_METHOD(JobU_HttpPayload, getMethod);
ZEND_METHOD(JobU_HttpPayload, setMethod);
ZEND_METHOD(JobU_HttpPayload, addHeader);
ZEND_METHOD(JobU_HttpPayload, getHeaders);
ZEND_METHOD(JobU_HttpPayload, clearHeader);
ZEND_METHOD(JobU_HttpPayload, setBody);
ZEND_METHOD(JobU_HttpPayload, getBody);
ZEND_METHOD(JobU_HttpPayload, clearBody);
ZEND_METHOD(JobU_HttpPayload, setJsonBody);
ZEND_METHOD(JobU_HttpPayload, getExpectedStatuses);
ZEND_METHOD(JobU_HttpPayload, setExpectedStatuses);
ZEND_METHOD(JobU_HttpHeader, __construct);
ZEND_METHOD(JobU_HttpHeader, getName);
ZEND_METHOD(JobU_HttpHeader, getValue);
ZEND_METHOD(JobU_HttpHeader, isSensitive);
ZEND_METHOD(JobU_OnceSchedule, __construct);
ZEND_METHOD(JobU_OnceSchedule, now);
ZEND_METHOD(JobU_OnceSchedule, isNow);
ZEND_METHOD(JobU_OnceSchedule, getAt);
ZEND_METHOD(JobU_CronSchedule, __construct);
ZEND_METHOD(JobU_CronSchedule, getExpression);
ZEND_METHOD(JobU_CronSchedule, getTimezone);
ZEND_METHOD(JobU_JobAttributes, __construct);
ZEND_METHOD(JobU_JobAttributes, get);
ZEND_METHOD(JobU_JobAttributes, has);
ZEND_METHOD(JobU_JobAttributes, set);
ZEND_METHOD(JobU_JobAttributes, getTimeout);
ZEND_METHOD(JobU_JobAttributes, setTimeout);
ZEND_METHOD(JobU_JobAttributes, getOutputCapture);
ZEND_METHOD(JobU_JobAttributes, setOutputCapture);
ZEND_METHOD(JobU_JobAttributes, getRetryInitialDelay);
ZEND_METHOD(JobU_JobAttributes, setRetryInitialDelay);
ZEND_METHOD(JobU_JobAttributes, getRetryMaxDelay);
ZEND_METHOD(JobU_JobAttributes, setRetryMaxDelay);
ZEND_METHOD(JobU_JobAttributes, getRetryMaxAttempts);
ZEND_METHOD(JobU_JobAttributes, setRetryMaxAttempts);
ZEND_METHOD(JobU_JobAttributes, getRetryJitter);
ZEND_METHOD(JobU_JobAttributes, setRetryJitter);
ZEND_METHOD(JobU_JobAttributes, getRetryMultiplier);
ZEND_METHOD(JobU_JobAttributes, setRetryMultiplier);
ZEND_METHOD(JobU_JobAttributes, getRetryMode);
ZEND_METHOD(JobU_JobAttributes, setRetryMode);
ZEND_METHOD(JobU_JobAttributes, getRetryStrategy);
ZEND_METHOD(JobU_JobAttributes, setRetryStrategy);
ZEND_METHOD(JobU_CliJobAttributes, getRetryExitCodes);
ZEND_METHOD(JobU_CliJobAttributes, setRetryExitCodes);
ZEND_METHOD(JobU_CliJobAttributes, getTerminationGrace);
ZEND_METHOD(JobU_CliJobAttributes, setTerminationGrace);
ZEND_METHOD(JobU_CliJobAttributes, getStdoutLimit);
ZEND_METHOD(JobU_CliJobAttributes, setStdoutLimit);
ZEND_METHOD(JobU_CliJobAttributes, getStderrLimit);
ZEND_METHOD(JobU_CliJobAttributes, setStderrLimit);
ZEND_METHOD(JobU_HttpJobAttributes, getFollowRedirects);
ZEND_METHOD(JobU_HttpJobAttributes, setFollowRedirects);
ZEND_METHOD(JobU_HttpJobAttributes, getIdempotencyKey);
ZEND_METHOD(JobU_HttpJobAttributes, setIdempotencyKey);
ZEND_METHOD(JobU_HttpJobAttributes, getTlsVerify);
ZEND_METHOD(JobU_HttpJobAttributes, setTlsVerify);
ZEND_METHOD(JobU_HttpJobAttributes, getMaxRedirects);
ZEND_METHOD(JobU_HttpJobAttributes, setMaxRedirects);
ZEND_METHOD(JobU_HttpJobAttributes, getHttpBodyLimit);
ZEND_METHOD(JobU_HttpJobAttributes, setHttpBodyLimit);
ZEND_METHOD(JobU_HttpJobAttributes, getHttpHeadersLimit);
ZEND_METHOD(JobU_HttpJobAttributes, setHttpHeadersLimit);
ZEND_METHOD(JobU_HttpJobAttributes, getRetryErrors);
ZEND_METHOD(JobU_HttpJobAttributes, setRetryErrors);
ZEND_METHOD(JobU_HttpJobAttributes, getRetryStatuses);
ZEND_METHOD(JobU_HttpJobAttributes, setRetryStatuses);
ZEND_METHOD(JobU_QueueDefaults, __construct);
ZEND_METHOD(JobU_QueueDefaults, get);
ZEND_METHOD(JobU_QueueDefaults, has);
ZEND_METHOD(JobU_QueueDefaults, set);
ZEND_METHOD(JobU_QueueDefaults, clear);
ZEND_METHOD(JobU_QueueDefaults, cli);
ZEND_METHOD(JobU_QueueDefaults, http);
ZEND_METHOD(JobU_Client, __construct);
ZEND_METHOD(JobU_Client, close);
ZEND_METHOD(JobU_Client, listQueues);
ZEND_METHOD(JobU_Client, listJobs);
ZEND_METHOD(JobU_Client, getJob);
ZEND_METHOD(JobU_Client, getQueue);
ZEND_METHOD(JobU_Client, getQueueById);
ZEND_METHOD(JobU_Client, createQueue);
ZEND_METHOD(JobU_Queue, listJobs);
ZEND_METHOD(JobU_Queue, createCliJob);
ZEND_METHOD(JobU_Queue, createHttpJob);
ZEND_METHOD(JobU_Queue, __construct);
ZEND_METHOD(JobU_Queue, getId);
ZEND_METHOD(JobU_Queue, getRevision);
ZEND_METHOD(JobU_Queue, getName);
ZEND_METHOD(JobU_Queue, setName);
ZEND_METHOD(JobU_Queue, getState);
ZEND_METHOD(JobU_Queue, getWeight);
ZEND_METHOD(JobU_Queue, setWeight);
ZEND_METHOD(JobU_Queue, getConcurrencyLimit);
ZEND_METHOD(JobU_Queue, setConcurrencyLimit);
ZEND_METHOD(JobU_Queue, getRecoveryPolicy);
ZEND_METHOD(JobU_Queue, setRecoveryPolicy);
ZEND_METHOD(JobU_Queue, getHistoryRetention);
ZEND_METHOD(JobU_Queue, setHistoryRetention);
ZEND_METHOD(JobU_Queue, getRunnableWaitWarning);
ZEND_METHOD(JobU_Queue, setRunnableWaitWarning);
ZEND_METHOD(JobU_Queue, getCreatedAt);
ZEND_METHOD(JobU_Queue, getUpdatedAt);
ZEND_METHOD(JobU_Queue, getDeletedAt);
ZEND_METHOD(JobU_Queue, isDirty);
ZEND_METHOD(JobU_Queue, isDeleted);
ZEND_METHOD(JobU_Queue, getDefaults);
ZEND_METHOD(JobU_Queue, defaults);
ZEND_METHOD(JobU_Queue, setDefaults);
ZEND_METHOD(JobU_Queue, save);
ZEND_METHOD(JobU_Queue, refresh);
ZEND_METHOD(JobU_Queue, discardChanges);
ZEND_METHOD(JobU_Queue, suspend);
ZEND_METHOD(JobU_Queue, resume);
ZEND_METHOD(JobU_Queue, delete);
ZEND_METHOD(JobU_Exception, getErrorCode);
ZEND_METHOD(JobU_Exception, getCategory);
ZEND_METHOD(JobU_OutcomeUnknownException, getOperation);
ZEND_METHOD(JobU_OutcomeUnknownException, getIdempotencyKey);
ZEND_METHOD(JobU_TimeRange, __construct);
ZEND_METHOD(JobU_TimeRange, getFrom);
ZEND_METHOD(JobU_TimeRange, getTo);
ZEND_METHOD(JobU_RunQuery, __construct);
ZEND_METHOD(JobU_RunQuery, getQueueId);
ZEND_METHOD(JobU_RunQuery, getJobId);
ZEND_METHOD(JobU_RunQuery, getState);
ZEND_METHOD(JobU_RunQuery, getStates);
ZEND_METHOD(JobU_RunQuery, getOrigin);
ZEND_METHOD(JobU_RunQuery, getType);
ZEND_METHOD(JobU_RunQuery, getPlanned);
ZEND_METHOD(JobU_RunQuery, getStarted);
ZEND_METHOD(JobU_RunQuery, getCompleted);
ZEND_METHOD(JobU_RunQuery, getLimit);
ZEND_METHOD(JobU_StatisticsQuery, __construct);
ZEND_METHOD(JobU_StatisticsQuery, getQueueId);
ZEND_METHOD(JobU_StatisticsQuery, getJobId);
ZEND_METHOD(JobU_StatisticsQuery, getType);
ZEND_METHOD(JobU_StatisticsQuery, getOrigin);
ZEND_METHOD(JobU_StatisticsQuery, getPlanned);
ZEND_METHOD(JobU_StatisticsQuery, getGroupBy);
ZEND_METHOD(JobU_StatisticsQuery, getLimit);
ZEND_METHOD(JobU_SystemInfo, __construct);
ZEND_METHOD(JobU_SystemInfo, getDaemonVersion);
ZEND_METHOD(JobU_SystemInfo, getApiMajor);
ZEND_METHOD(JobU_SystemInfo, getApiMinor);
ZEND_METHOD(JobU_SystemInfo, getCapabilities);
ZEND_METHOD(JobU_SystemInfo, hasCapability);
ZEND_METHOD(JobU_SecretMetadata, __construct);
ZEND_METHOD(JobU_SecretMetadata, getName);
ZEND_METHOD(JobU_SecretMetadata, getCreatedAt);
ZEND_METHOD(JobU_SecretMetadata, getUpdatedAt);
ZEND_METHOD(JobU_ExecutionResult, __construct);
ZEND_METHOD(JobU_ExecutionResult, getType);
ZEND_METHOD(JobU_ExecutionResult, getOutcome);
ZEND_METHOD(JobU_ExecutionResult, getReason);
ZEND_METHOD(JobU_ExecutionResult, getErrorCode);
ZEND_METHOD(JobU_ExecutionResult, getOutcomeUnknown);
ZEND_METHOD(JobU_ExecutionResult, has);
ZEND_METHOD(JobU_ExecutionResult, get);
ZEND_METHOD(JobU_ExecutionResult, getCapturedBytes);
ZEND_METHOD(JobU_ExecutionResult, getTotalBytes);
ZEND_METHOD(JobU_ExecutionResult, isTruncated);
ZEND_METHOD(JobU_CliExecutionResult, getExitCode);
ZEND_METHOD(JobU_CliExecutionResult, getSignal);
ZEND_METHOD(JobU_CliExecutionResult, getCaptureLost);
ZEND_METHOD(JobU_HttpExecutionResult, getStatus);
ZEND_METHOD(JobU_HttpExecutionResult, getDuration);
ZEND_METHOD(JobU_HttpExecutionResult, getRedirects);
ZEND_METHOD(JobU_HttpExecutionResult, getTlsVerified);
ZEND_METHOD(JobU_HttpExecutionResult, getErrorCategory);
ZEND_METHOD(JobU_HttpExecutionResult, getRetryAfterAccepted);
ZEND_METHOD(JobU_HttpExecutionResult, getRetryAfterClamped);
ZEND_METHOD(JobU_OutputChunk, __construct);
ZEND_METHOD(JobU_OutputChunk, getRunId);
ZEND_METHOD(JobU_OutputChunk, getAttemptNumber);
ZEND_METHOD(JobU_OutputChunk, getChannel);
ZEND_METHOD(JobU_OutputChunk, getStatus);
ZEND_METHOD(JobU_OutputChunk, getOffset);
ZEND_METHOD(JobU_OutputChunk, getBytesReturned);
ZEND_METHOD(JobU_OutputChunk, getNextOffset);
ZEND_METHOD(JobU_OutputChunk, getRetainedBytes);
ZEND_METHOD(JobU_OutputChunk, getTotalBytes);
ZEND_METHOD(JobU_OutputChunk, getOmittedBytes);
ZEND_METHOD(JobU_OutputChunk, isTruncated);
ZEND_METHOD(JobU_OutputChunk, isCaptureLost);
ZEND_METHOD(JobU_OutputChunk, getData);
ZEND_METHOD(JobU_Job, __construct);
ZEND_METHOD(JobU_Job, getId);
ZEND_METHOD(JobU_Job, getQueueId);
ZEND_METHOD(JobU_Job, getRevision);
ZEND_METHOD(JobU_Job, getName);
ZEND_METHOD(JobU_Job, getPriority);
ZEND_METHOD(JobU_Job, getState);
ZEND_METHOD(JobU_Job, getType);
ZEND_METHOD(JobU_Job, getCreatedAt);
ZEND_METHOD(JobU_Job, getUpdatedAt);
ZEND_METHOD(JobU_Job, getDeletedAt);
ZEND_METHOD(JobU_Job, getSchedule);
ZEND_METHOD(JobU_Job, getAttributes);
ZEND_METHOD(JobU_Job, attributes);
ZEND_METHOD(JobU_Job, getPayload);
ZEND_METHOD(JobU_Job, payload);
ZEND_METHOD(JobU_Job, isDirty);
ZEND_METHOD(JobU_Job, isDeleted);
ZEND_METHOD(JobU_Job, setName);
ZEND_METHOD(JobU_Job, setPriority);
ZEND_METHOD(JobU_Job, setSchedule);
ZEND_METHOD(JobU_Job, setAttributes);
ZEND_METHOD(JobU_Job, setPayload);
ZEND_METHOD(JobU_Job, save);
ZEND_METHOD(JobU_Job, refresh);
ZEND_METHOD(JobU_Job, discardChanges);
ZEND_METHOD(JobU_Job, suspend);
ZEND_METHOD(JobU_Job, resume);
ZEND_METHOD(JobU_Job, move);
ZEND_METHOD(JobU_Job, delete);
ZEND_METHOD(JobU_Job, runNow);
ZEND_METHOD(JobU_Run, __construct);
ZEND_METHOD(JobU_Run, getId);
ZEND_METHOD(JobU_Run, getJobId);
ZEND_METHOD(JobU_Run, getQueueId);
ZEND_METHOD(JobU_Run, getJobRevision);
ZEND_METHOD(JobU_Run, getPriority);
ZEND_METHOD(JobU_Run, getOrigin);
ZEND_METHOD(JobU_Run, getType);
ZEND_METHOD(JobU_Run, getState);
ZEND_METHOD(JobU_Run, isScheduleOwned);
ZEND_METHOD(JobU_Run, getPlannedAt);
ZEND_METHOD(JobU_Run, getRunnableAt);
ZEND_METHOD(JobU_Run, getStartedAt);
ZEND_METHOD(JobU_Run, getCompletedAt);
ZEND_METHOD(JobU_Run, getAttributes);
ZEND_METHOD(JobU_Run, getPayload);
ZEND_METHOD(JobU_Run, getResult);
ZEND_METHOD(JobU_Page, __construct);
ZEND_METHOD(JobU_Page, hasNext);
ZEND_METHOD(JobU_Page, next);
ZEND_METHOD(JobU_Page, items);

static const zend_function_entry class_JobU_Duration_methods[] = {
	ZEND_ME(JobU_Duration, __construct, arginfo_class_JobU_Duration___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_Duration, getMilliseconds, arginfo_class_JobU_Duration_getMilliseconds, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Duration, milliseconds, arginfo_class_JobU_Duration_milliseconds, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, seconds, arginfo_class_JobU_Duration_seconds, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, minutes, arginfo_class_JobU_Duration_minutes, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, hours, arginfo_class_JobU_Duration_hours, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, days, arginfo_class_JobU_Duration_days, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_SecretReference_methods[] = {
	ZEND_ME(JobU_SecretReference, __construct, arginfo_class_JobU_SecretReference___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SecretReference, getName, arginfo_class_JobU_SecretReference_getName, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_CliPayload_methods[] = {
	ZEND_ME(JobU_CliPayload, __construct, arginfo_class_JobU_CliPayload___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, setCommand, arginfo_class_JobU_CliPayload_setCommand, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, getExecutable, arginfo_class_JobU_CliPayload_getExecutable, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, setExecutable, arginfo_class_JobU_CliPayload_setExecutable, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, getArguments, arginfo_class_JobU_CliPayload_getArguments, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, setArguments, arginfo_class_JobU_CliPayload_setArguments, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, addArgument, arginfo_class_JobU_CliPayload_addArgument, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, getEnvironment, arginfo_class_JobU_CliPayload_getEnvironment, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, setEnvironment, arginfo_class_JobU_CliPayload_setEnvironment, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, unsetEnvironment, arginfo_class_JobU_CliPayload_unsetEnvironment, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, clearEnvironment, arginfo_class_JobU_CliPayload_clearEnvironment, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, getWorkingDirectory, arginfo_class_JobU_CliPayload_getWorkingDirectory, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, setWorkingDirectory, arginfo_class_JobU_CliPayload_setWorkingDirectory, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, getExpectedExitCodes, arginfo_class_JobU_CliPayload_getExpectedExitCodes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliPayload, setExpectedExitCodes, arginfo_class_JobU_CliPayload_setExpectedExitCodes, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_HttpPayload_methods[] = {
	ZEND_ME(JobU_HttpPayload, __construct, arginfo_class_JobU_HttpPayload___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, getUrl, arginfo_class_JobU_HttpPayload_getUrl, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, setUrl, arginfo_class_JobU_HttpPayload_setUrl, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, getMethod, arginfo_class_JobU_HttpPayload_getMethod, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, setMethod, arginfo_class_JobU_HttpPayload_setMethod, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, addHeader, arginfo_class_JobU_HttpPayload_addHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, getHeaders, arginfo_class_JobU_HttpPayload_getHeaders, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, clearHeader, arginfo_class_JobU_HttpPayload_clearHeader, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, setBody, arginfo_class_JobU_HttpPayload_setBody, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, getBody, arginfo_class_JobU_HttpPayload_getBody, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, clearBody, arginfo_class_JobU_HttpPayload_clearBody, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, setJsonBody, arginfo_class_JobU_HttpPayload_setJsonBody, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, getExpectedStatuses, arginfo_class_JobU_HttpPayload_getExpectedStatuses, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpPayload, setExpectedStatuses, arginfo_class_JobU_HttpPayload_setExpectedStatuses, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_HttpHeader_methods[] = {
	ZEND_ME(JobU_HttpHeader, __construct, arginfo_class_JobU_HttpHeader___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_HttpHeader, getName, arginfo_class_JobU_HttpHeader_getName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpHeader, getValue, arginfo_class_JobU_HttpHeader_getValue, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpHeader, isSensitive, arginfo_class_JobU_HttpHeader_isSensitive, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_OnceSchedule_methods[] = {
	ZEND_ME(JobU_OnceSchedule, __construct, arginfo_class_JobU_OnceSchedule___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OnceSchedule, now, arginfo_class_JobU_OnceSchedule_now, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_OnceSchedule, isNow, arginfo_class_JobU_OnceSchedule_isNow, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OnceSchedule, getAt, arginfo_class_JobU_OnceSchedule_getAt, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_CronSchedule_methods[] = {
	ZEND_ME(JobU_CronSchedule, __construct, arginfo_class_JobU_CronSchedule___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CronSchedule, getExpression, arginfo_class_JobU_CronSchedule_getExpression, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CronSchedule, getTimezone, arginfo_class_JobU_CronSchedule_getTimezone, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_JobAttributes_methods[] = {
	ZEND_ME(JobU_JobAttributes, __construct, arginfo_class_JobU_JobAttributes___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, get, arginfo_class_JobU_JobAttributes_get, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, has, arginfo_class_JobU_JobAttributes_has, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, set, arginfo_class_JobU_JobAttributes_set, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getTimeout, arginfo_class_JobU_JobAttributes_getTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setTimeout, arginfo_class_JobU_JobAttributes_setTimeout, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getOutputCapture, arginfo_class_JobU_JobAttributes_getOutputCapture, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setOutputCapture, arginfo_class_JobU_JobAttributes_setOutputCapture, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryInitialDelay, arginfo_class_JobU_JobAttributes_getRetryInitialDelay, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryInitialDelay, arginfo_class_JobU_JobAttributes_setRetryInitialDelay, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryMaxDelay, arginfo_class_JobU_JobAttributes_getRetryMaxDelay, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryMaxDelay, arginfo_class_JobU_JobAttributes_setRetryMaxDelay, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryMaxAttempts, arginfo_class_JobU_JobAttributes_getRetryMaxAttempts, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryMaxAttempts, arginfo_class_JobU_JobAttributes_setRetryMaxAttempts, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryJitter, arginfo_class_JobU_JobAttributes_getRetryJitter, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryJitter, arginfo_class_JobU_JobAttributes_setRetryJitter, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryMultiplier, arginfo_class_JobU_JobAttributes_getRetryMultiplier, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryMultiplier, arginfo_class_JobU_JobAttributes_setRetryMultiplier, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryMode, arginfo_class_JobU_JobAttributes_getRetryMode, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryMode, arginfo_class_JobU_JobAttributes_setRetryMode, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, getRetryStrategy, arginfo_class_JobU_JobAttributes_getRetryStrategy, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_JobAttributes, setRetryStrategy, arginfo_class_JobU_JobAttributes_setRetryStrategy, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_CliJobAttributes_methods[] = {
	ZEND_ME(JobU_CliJobAttributes, getRetryExitCodes, arginfo_class_JobU_CliJobAttributes_getRetryExitCodes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, setRetryExitCodes, arginfo_class_JobU_CliJobAttributes_setRetryExitCodes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, getTerminationGrace, arginfo_class_JobU_CliJobAttributes_getTerminationGrace, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, setTerminationGrace, arginfo_class_JobU_CliJobAttributes_setTerminationGrace, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, getStdoutLimit, arginfo_class_JobU_CliJobAttributes_getStdoutLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, setStdoutLimit, arginfo_class_JobU_CliJobAttributes_setStdoutLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, getStderrLimit, arginfo_class_JobU_CliJobAttributes_getStderrLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliJobAttributes, setStderrLimit, arginfo_class_JobU_CliJobAttributes_setStderrLimit, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_HttpJobAttributes_methods[] = {
	ZEND_ME(JobU_HttpJobAttributes, getFollowRedirects, arginfo_class_JobU_HttpJobAttributes_getFollowRedirects, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setFollowRedirects, arginfo_class_JobU_HttpJobAttributes_setFollowRedirects, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getIdempotencyKey, arginfo_class_JobU_HttpJobAttributes_getIdempotencyKey, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setIdempotencyKey, arginfo_class_JobU_HttpJobAttributes_setIdempotencyKey, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getTlsVerify, arginfo_class_JobU_HttpJobAttributes_getTlsVerify, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setTlsVerify, arginfo_class_JobU_HttpJobAttributes_setTlsVerify, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getMaxRedirects, arginfo_class_JobU_HttpJobAttributes_getMaxRedirects, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setMaxRedirects, arginfo_class_JobU_HttpJobAttributes_setMaxRedirects, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getHttpBodyLimit, arginfo_class_JobU_HttpJobAttributes_getHttpBodyLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setHttpBodyLimit, arginfo_class_JobU_HttpJobAttributes_setHttpBodyLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getHttpHeadersLimit, arginfo_class_JobU_HttpJobAttributes_getHttpHeadersLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setHttpHeadersLimit, arginfo_class_JobU_HttpJobAttributes_setHttpHeadersLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getRetryErrors, arginfo_class_JobU_HttpJobAttributes_getRetryErrors, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setRetryErrors, arginfo_class_JobU_HttpJobAttributes_setRetryErrors, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, getRetryStatuses, arginfo_class_JobU_HttpJobAttributes_getRetryStatuses, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpJobAttributes, setRetryStatuses, arginfo_class_JobU_HttpJobAttributes_setRetryStatuses, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_QueueDefaults_methods[] = {
	ZEND_ME(JobU_QueueDefaults, __construct, arginfo_class_JobU_QueueDefaults___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_QueueDefaults, get, arginfo_class_JobU_QueueDefaults_get, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_QueueDefaults, has, arginfo_class_JobU_QueueDefaults_has, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_QueueDefaults, set, arginfo_class_JobU_QueueDefaults_set, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_QueueDefaults, clear, arginfo_class_JobU_QueueDefaults_clear, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_QueueDefaults, cli, arginfo_class_JobU_QueueDefaults_cli, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_QueueDefaults, http, arginfo_class_JobU_QueueDefaults_http, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_Client_methods[] = {
	ZEND_ME(JobU_Client, __construct, arginfo_class_JobU_Client___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, close, arginfo_class_JobU_Client_close, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, listQueues, arginfo_class_JobU_Client_listQueues, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, listJobs, arginfo_class_JobU_Client_listJobs, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, getJob, arginfo_class_JobU_Client_getJob, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, getQueue, arginfo_class_JobU_Client_getQueue, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, getQueueById, arginfo_class_JobU_Client_getQueueById, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Client, createQueue, arginfo_class_JobU_Client_createQueue, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_Queue_methods[] = {
	ZEND_ME(JobU_Queue, listJobs, arginfo_class_JobU_Queue_listJobs, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, createCliJob, arginfo_class_JobU_Queue_createCliJob, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, createHttpJob, arginfo_class_JobU_Queue_createHttpJob, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, __construct, arginfo_class_JobU_Queue___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_Queue, getId, arginfo_class_JobU_Queue_getId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getRevision, arginfo_class_JobU_Queue_getRevision, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getName, arginfo_class_JobU_Queue_getName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setName, arginfo_class_JobU_Queue_setName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getState, arginfo_class_JobU_Queue_getState, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getWeight, arginfo_class_JobU_Queue_getWeight, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setWeight, arginfo_class_JobU_Queue_setWeight, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getConcurrencyLimit, arginfo_class_JobU_Queue_getConcurrencyLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setConcurrencyLimit, arginfo_class_JobU_Queue_setConcurrencyLimit, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getRecoveryPolicy, arginfo_class_JobU_Queue_getRecoveryPolicy, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setRecoveryPolicy, arginfo_class_JobU_Queue_setRecoveryPolicy, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getHistoryRetention, arginfo_class_JobU_Queue_getHistoryRetention, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setHistoryRetention, arginfo_class_JobU_Queue_setHistoryRetention, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getRunnableWaitWarning, arginfo_class_JobU_Queue_getRunnableWaitWarning, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setRunnableWaitWarning, arginfo_class_JobU_Queue_setRunnableWaitWarning, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getCreatedAt, arginfo_class_JobU_Queue_getCreatedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getUpdatedAt, arginfo_class_JobU_Queue_getUpdatedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getDeletedAt, arginfo_class_JobU_Queue_getDeletedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, isDirty, arginfo_class_JobU_Queue_isDirty, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, isDeleted, arginfo_class_JobU_Queue_isDeleted, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, getDefaults, arginfo_class_JobU_Queue_getDefaults, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, defaults, arginfo_class_JobU_Queue_defaults, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, setDefaults, arginfo_class_JobU_Queue_setDefaults, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, save, arginfo_class_JobU_Queue_save, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, refresh, arginfo_class_JobU_Queue_refresh, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, discardChanges, arginfo_class_JobU_Queue_discardChanges, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, suspend, arginfo_class_JobU_Queue_suspend, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, resume, arginfo_class_JobU_Queue_resume, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Queue, delete, arginfo_class_JobU_Queue_delete, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_Exception_methods[] = {
	ZEND_ME(JobU_Exception, getErrorCode, arginfo_class_JobU_Exception_getErrorCode, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Exception, getCategory, arginfo_class_JobU_Exception_getCategory, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_OutcomeUnknownException_methods[] = {
	ZEND_ME(JobU_OutcomeUnknownException, getOperation, arginfo_class_JobU_OutcomeUnknownException_getOperation, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutcomeUnknownException, getIdempotencyKey, arginfo_class_JobU_OutcomeUnknownException_getIdempotencyKey, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_TimeRange_methods[] = {
	ZEND_ME(JobU_TimeRange, __construct, arginfo_class_JobU_TimeRange___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_TimeRange, getFrom, arginfo_class_JobU_TimeRange_getFrom, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_TimeRange, getTo, arginfo_class_JobU_TimeRange_getTo, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_RunQuery_methods[] = {
	ZEND_ME(JobU_RunQuery, __construct, arginfo_class_JobU_RunQuery___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getQueueId, arginfo_class_JobU_RunQuery_getQueueId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getJobId, arginfo_class_JobU_RunQuery_getJobId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getState, arginfo_class_JobU_RunQuery_getState, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getStates, arginfo_class_JobU_RunQuery_getStates, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getOrigin, arginfo_class_JobU_RunQuery_getOrigin, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getType, arginfo_class_JobU_RunQuery_getType, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getPlanned, arginfo_class_JobU_RunQuery_getPlanned, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getStarted, arginfo_class_JobU_RunQuery_getStarted, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getCompleted, arginfo_class_JobU_RunQuery_getCompleted, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_RunQuery, getLimit, arginfo_class_JobU_RunQuery_getLimit, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_StatisticsQuery_methods[] = {
	ZEND_ME(JobU_StatisticsQuery, __construct, arginfo_class_JobU_StatisticsQuery___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getQueueId, arginfo_class_JobU_StatisticsQuery_getQueueId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getJobId, arginfo_class_JobU_StatisticsQuery_getJobId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getType, arginfo_class_JobU_StatisticsQuery_getType, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getOrigin, arginfo_class_JobU_StatisticsQuery_getOrigin, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getPlanned, arginfo_class_JobU_StatisticsQuery_getPlanned, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getGroupBy, arginfo_class_JobU_StatisticsQuery_getGroupBy, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_StatisticsQuery, getLimit, arginfo_class_JobU_StatisticsQuery_getLimit, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_SystemInfo_methods[] = {
	ZEND_ME(JobU_SystemInfo, __construct, arginfo_class_JobU_SystemInfo___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_SystemInfo, getDaemonVersion, arginfo_class_JobU_SystemInfo_getDaemonVersion, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SystemInfo, getApiMajor, arginfo_class_JobU_SystemInfo_getApiMajor, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SystemInfo, getApiMinor, arginfo_class_JobU_SystemInfo_getApiMinor, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SystemInfo, getCapabilities, arginfo_class_JobU_SystemInfo_getCapabilities, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SystemInfo, hasCapability, arginfo_class_JobU_SystemInfo_hasCapability, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_SecretMetadata_methods[] = {
	ZEND_ME(JobU_SecretMetadata, __construct, arginfo_class_JobU_SecretMetadata___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_SecretMetadata, getName, arginfo_class_JobU_SecretMetadata_getName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SecretMetadata, getCreatedAt, arginfo_class_JobU_SecretMetadata_getCreatedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_SecretMetadata, getUpdatedAt, arginfo_class_JobU_SecretMetadata_getUpdatedAt, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_ExecutionResult_methods[] = {
	ZEND_ME(JobU_ExecutionResult, __construct, arginfo_class_JobU_ExecutionResult___construct, ZEND_ACC_PRIVATE|ZEND_ACC_FINAL)
	ZEND_ME(JobU_ExecutionResult, getType, arginfo_class_JobU_ExecutionResult_getType, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, getOutcome, arginfo_class_JobU_ExecutionResult_getOutcome, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, getReason, arginfo_class_JobU_ExecutionResult_getReason, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, getErrorCode, arginfo_class_JobU_ExecutionResult_getErrorCode, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, getOutcomeUnknown, arginfo_class_JobU_ExecutionResult_getOutcomeUnknown, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, has, arginfo_class_JobU_ExecutionResult_has, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, get, arginfo_class_JobU_ExecutionResult_get, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, getCapturedBytes, arginfo_class_JobU_ExecutionResult_getCapturedBytes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, getTotalBytes, arginfo_class_JobU_ExecutionResult_getTotalBytes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_ExecutionResult, isTruncated, arginfo_class_JobU_ExecutionResult_isTruncated, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_CliExecutionResult_methods[] = {
	ZEND_ME(JobU_CliExecutionResult, getExitCode, arginfo_class_JobU_CliExecutionResult_getExitCode, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliExecutionResult, getSignal, arginfo_class_JobU_CliExecutionResult_getSignal, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_CliExecutionResult, getCaptureLost, arginfo_class_JobU_CliExecutionResult_getCaptureLost, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_HttpExecutionResult_methods[] = {
	ZEND_ME(JobU_HttpExecutionResult, getStatus, arginfo_class_JobU_HttpExecutionResult_getStatus, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpExecutionResult, getDuration, arginfo_class_JobU_HttpExecutionResult_getDuration, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpExecutionResult, getRedirects, arginfo_class_JobU_HttpExecutionResult_getRedirects, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpExecutionResult, getTlsVerified, arginfo_class_JobU_HttpExecutionResult_getTlsVerified, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpExecutionResult, getErrorCategory, arginfo_class_JobU_HttpExecutionResult_getErrorCategory, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpExecutionResult, getRetryAfterAccepted, arginfo_class_JobU_HttpExecutionResult_getRetryAfterAccepted, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_HttpExecutionResult, getRetryAfterClamped, arginfo_class_JobU_HttpExecutionResult_getRetryAfterClamped, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_OutputChunk_methods[] = {
	ZEND_ME(JobU_OutputChunk, __construct, arginfo_class_JobU_OutputChunk___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_OutputChunk, getRunId, arginfo_class_JobU_OutputChunk_getRunId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getAttemptNumber, arginfo_class_JobU_OutputChunk_getAttemptNumber, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getChannel, arginfo_class_JobU_OutputChunk_getChannel, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getStatus, arginfo_class_JobU_OutputChunk_getStatus, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getOffset, arginfo_class_JobU_OutputChunk_getOffset, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getBytesReturned, arginfo_class_JobU_OutputChunk_getBytesReturned, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getNextOffset, arginfo_class_JobU_OutputChunk_getNextOffset, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getRetainedBytes, arginfo_class_JobU_OutputChunk_getRetainedBytes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getTotalBytes, arginfo_class_JobU_OutputChunk_getTotalBytes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getOmittedBytes, arginfo_class_JobU_OutputChunk_getOmittedBytes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, isTruncated, arginfo_class_JobU_OutputChunk_isTruncated, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, isCaptureLost, arginfo_class_JobU_OutputChunk_isCaptureLost, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_OutputChunk, getData, arginfo_class_JobU_OutputChunk_getData, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_Job_methods[] = {
	ZEND_ME(JobU_Job, __construct, arginfo_class_JobU_Job___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_Job, getId, arginfo_class_JobU_Job_getId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getQueueId, arginfo_class_JobU_Job_getQueueId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getRevision, arginfo_class_JobU_Job_getRevision, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getName, arginfo_class_JobU_Job_getName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getPriority, arginfo_class_JobU_Job_getPriority, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getState, arginfo_class_JobU_Job_getState, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getType, arginfo_class_JobU_Job_getType, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getCreatedAt, arginfo_class_JobU_Job_getCreatedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getUpdatedAt, arginfo_class_JobU_Job_getUpdatedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getDeletedAt, arginfo_class_JobU_Job_getDeletedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getSchedule, arginfo_class_JobU_Job_getSchedule, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getAttributes, arginfo_class_JobU_Job_getAttributes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, attributes, arginfo_class_JobU_Job_attributes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, getPayload, arginfo_class_JobU_Job_getPayload, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, payload, arginfo_class_JobU_Job_payload, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, isDirty, arginfo_class_JobU_Job_isDirty, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, isDeleted, arginfo_class_JobU_Job_isDeleted, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, setName, arginfo_class_JobU_Job_setName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, setPriority, arginfo_class_JobU_Job_setPriority, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, setSchedule, arginfo_class_JobU_Job_setSchedule, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, setAttributes, arginfo_class_JobU_Job_setAttributes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, setPayload, arginfo_class_JobU_Job_setPayload, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, save, arginfo_class_JobU_Job_save, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, refresh, arginfo_class_JobU_Job_refresh, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, discardChanges, arginfo_class_JobU_Job_discardChanges, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, suspend, arginfo_class_JobU_Job_suspend, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, resume, arginfo_class_JobU_Job_resume, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, move, arginfo_class_JobU_Job_move, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, delete, arginfo_class_JobU_Job_delete, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Job, runNow, arginfo_class_JobU_Job_runNow, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_Run_methods[] = {
	ZEND_ME(JobU_Run, __construct, arginfo_class_JobU_Run___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_Run, getId, arginfo_class_JobU_Run_getId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getJobId, arginfo_class_JobU_Run_getJobId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getQueueId, arginfo_class_JobU_Run_getQueueId, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getJobRevision, arginfo_class_JobU_Run_getJobRevision, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getPriority, arginfo_class_JobU_Run_getPriority, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getOrigin, arginfo_class_JobU_Run_getOrigin, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getType, arginfo_class_JobU_Run_getType, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getState, arginfo_class_JobU_Run_getState, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, isScheduleOwned, arginfo_class_JobU_Run_isScheduleOwned, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getPlannedAt, arginfo_class_JobU_Run_getPlannedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getRunnableAt, arginfo_class_JobU_Run_getRunnableAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getStartedAt, arginfo_class_JobU_Run_getStartedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getCompletedAt, arginfo_class_JobU_Run_getCompletedAt, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getAttributes, arginfo_class_JobU_Run_getAttributes, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getPayload, arginfo_class_JobU_Run_getPayload, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Run, getResult, arginfo_class_JobU_Run_getResult, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobU_Page_methods[] = {
	ZEND_ME(JobU_Page, __construct, arginfo_class_JobU_Page___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_Page, hasNext, arginfo_class_JobU_Page_hasNext, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Page, next, arginfo_class_JobU_Page_next, ZEND_ACC_PUBLIC)
	ZEND_ME(JobU_Page, items, arginfo_class_JobU_Page_items, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_JobU_Duration(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Duration", class_JobU_Duration_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_SecretReference(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "SecretReference", class_JobU_SecretReference_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_CliPayload(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "CliPayload", class_JobU_CliPayload_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_HttpPayload(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "HttpPayload", class_JobU_HttpPayload_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_HttpHeader(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "HttpHeader", class_JobU_HttpHeader_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_Schedule(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Schedule", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_ABSTRACT);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_ABSTRACT;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_OnceSchedule(zend_class_entry *class_entry_JobU_Schedule)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "OnceSchedule", class_JobU_OnceSchedule_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Schedule, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Schedule);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_CronSchedule(zend_class_entry *class_entry_JobU_Schedule)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "CronSchedule", class_JobU_CronSchedule_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Schedule, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Schedule);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_JobAttributes(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "JobAttributes", class_JobU_JobAttributes_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, 0);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_CliJobAttributes(zend_class_entry *class_entry_JobU_JobAttributes)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "CliJobAttributes", class_JobU_CliJobAttributes_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_JobAttributes, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_JobAttributes);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_HttpJobAttributes(zend_class_entry *class_entry_JobU_JobAttributes)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "HttpJobAttributes", class_JobU_HttpJobAttributes_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_JobAttributes, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_JobAttributes);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_QueueDefaults(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "QueueDefaults", class_JobU_QueueDefaults_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_Client(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Client", class_JobU_Client_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_Queue(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Queue", class_JobU_Queue_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_QueueState(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\QueueState", IS_STRING, NULL);

	zval enum_case_Active_value;
	zend_string *enum_case_Active_value_str = zend_string_init("active", strlen("active"), 1);
	ZVAL_STR(&enum_case_Active_value, enum_case_Active_value_str);
	zend_enum_add_case_cstr(class_entry, "Active", &enum_case_Active_value);

	zval enum_case_Suspending_value;
	zend_string *enum_case_Suspending_value_str = zend_string_init("suspending", strlen("suspending"), 1);
	ZVAL_STR(&enum_case_Suspending_value, enum_case_Suspending_value_str);
	zend_enum_add_case_cstr(class_entry, "Suspending", &enum_case_Suspending_value);

	zval enum_case_Suspended_value;
	zend_string *enum_case_Suspended_value_str = zend_string_init("suspended", strlen("suspended"), 1);
	ZVAL_STR(&enum_case_Suspended_value, enum_case_Suspended_value_str);
	zend_enum_add_case_cstr(class_entry, "Suspended", &enum_case_Suspended_value);

	zval enum_case_Deleted_value;
	zend_string *enum_case_Deleted_value_str = zend_string_init("deleted", strlen("deleted"), 1);
	ZVAL_STR(&enum_case_Deleted_value, enum_case_Deleted_value_str);
	zend_enum_add_case_cstr(class_entry, "Deleted", &enum_case_Deleted_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_JobState(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\JobState", IS_STRING, NULL);

	zval enum_case_Active_value;
	zend_string *enum_case_Active_value_str = zend_string_init("active", strlen("active"), 1);
	ZVAL_STR(&enum_case_Active_value, enum_case_Active_value_str);
	zend_enum_add_case_cstr(class_entry, "Active", &enum_case_Active_value);

	zval enum_case_Suspending_value;
	zend_string *enum_case_Suspending_value_str = zend_string_init("suspending", strlen("suspending"), 1);
	ZVAL_STR(&enum_case_Suspending_value, enum_case_Suspending_value_str);
	zend_enum_add_case_cstr(class_entry, "Suspending", &enum_case_Suspending_value);

	zval enum_case_Suspended_value;
	zend_string *enum_case_Suspended_value_str = zend_string_init("suspended", strlen("suspended"), 1);
	ZVAL_STR(&enum_case_Suspended_value, enum_case_Suspended_value_str);
	zend_enum_add_case_cstr(class_entry, "Suspended", &enum_case_Suspended_value);

	zval enum_case_Deleted_value;
	zend_string *enum_case_Deleted_value_str = zend_string_init("deleted", strlen("deleted"), 1);
	ZVAL_STR(&enum_case_Deleted_value, enum_case_Deleted_value_str);
	zend_enum_add_case_cstr(class_entry, "Deleted", &enum_case_Deleted_value);

	zval enum_case_Succeeded_value;
	zend_string *enum_case_Succeeded_value_str = zend_string_init("succeeded", strlen("succeeded"), 1);
	ZVAL_STR(&enum_case_Succeeded_value, enum_case_Succeeded_value_str);
	zend_enum_add_case_cstr(class_entry, "Succeeded", &enum_case_Succeeded_value);

	zval enum_case_Failed_value;
	zend_string *enum_case_Failed_value_str = zend_string_init("failed", strlen("failed"), 1);
	ZVAL_STR(&enum_case_Failed_value, enum_case_Failed_value_str);
	zend_enum_add_case_cstr(class_entry, "Failed", &enum_case_Failed_value);

	zval enum_case_Cancelled_value;
	zend_string *enum_case_Cancelled_value_str = zend_string_init("cancelled", strlen("cancelled"), 1);
	ZVAL_STR(&enum_case_Cancelled_value, enum_case_Cancelled_value_str);
	zend_enum_add_case_cstr(class_entry, "Cancelled", &enum_case_Cancelled_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_RunState(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\RunState", IS_STRING, NULL);

	zval enum_case_Scheduled_value;
	zend_string *enum_case_Scheduled_value_str = zend_string_init("scheduled", strlen("scheduled"), 1);
	ZVAL_STR(&enum_case_Scheduled_value, enum_case_Scheduled_value_str);
	zend_enum_add_case_cstr(class_entry, "Scheduled", &enum_case_Scheduled_value);

	zval enum_case_Running_value;
	zend_string *enum_case_Running_value_str = zend_string_init("running", strlen("running"), 1);
	ZVAL_STR(&enum_case_Running_value, enum_case_Running_value_str);
	zend_enum_add_case_cstr(class_entry, "Running", &enum_case_Running_value);

	zval enum_case_RetryWait_value;
	zend_string *enum_case_RetryWait_value_str = zend_string_init("retry_wait", strlen("retry_wait"), 1);
	ZVAL_STR(&enum_case_RetryWait_value, enum_case_RetryWait_value_str);
	zend_enum_add_case_cstr(class_entry, "RetryWait", &enum_case_RetryWait_value);

	zval enum_case_Succeeded_value;
	zend_string *enum_case_Succeeded_value_str = zend_string_init("succeeded", strlen("succeeded"), 1);
	ZVAL_STR(&enum_case_Succeeded_value, enum_case_Succeeded_value_str);
	zend_enum_add_case_cstr(class_entry, "Succeeded", &enum_case_Succeeded_value);

	zval enum_case_Failed_value;
	zend_string *enum_case_Failed_value_str = zend_string_init("failed", strlen("failed"), 1);
	ZVAL_STR(&enum_case_Failed_value, enum_case_Failed_value_str);
	zend_enum_add_case_cstr(class_entry, "Failed", &enum_case_Failed_value);

	zval enum_case_Interrupted_value;
	zend_string *enum_case_Interrupted_value_str = zend_string_init("interrupted", strlen("interrupted"), 1);
	ZVAL_STR(&enum_case_Interrupted_value, enum_case_Interrupted_value_str);
	zend_enum_add_case_cstr(class_entry, "Interrupted", &enum_case_Interrupted_value);

	zval enum_case_Cancelled_value;
	zend_string *enum_case_Cancelled_value_str = zend_string_init("cancelled", strlen("cancelled"), 1);
	ZVAL_STR(&enum_case_Cancelled_value, enum_case_Cancelled_value_str);
	zend_enum_add_case_cstr(class_entry, "Cancelled", &enum_case_Cancelled_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_AttemptState(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\AttemptState", IS_STRING, NULL);

	zval enum_case_Pending_value;
	zend_string *enum_case_Pending_value_str = zend_string_init("pending", strlen("pending"), 1);
	ZVAL_STR(&enum_case_Pending_value, enum_case_Pending_value_str);
	zend_enum_add_case_cstr(class_entry, "Pending", &enum_case_Pending_value);

	zval enum_case_Running_value;
	zend_string *enum_case_Running_value_str = zend_string_init("running", strlen("running"), 1);
	ZVAL_STR(&enum_case_Running_value, enum_case_Running_value_str);
	zend_enum_add_case_cstr(class_entry, "Running", &enum_case_Running_value);

	zval enum_case_Completed_value;
	zend_string *enum_case_Completed_value_str = zend_string_init("completed", strlen("completed"), 1);
	ZVAL_STR(&enum_case_Completed_value, enum_case_Completed_value_str);
	zend_enum_add_case_cstr(class_entry, "Completed", &enum_case_Completed_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_AttemptOutcome(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\AttemptOutcome", IS_STRING, NULL);

	zval enum_case_Succeeded_value;
	zend_string *enum_case_Succeeded_value_str = zend_string_init("succeeded", strlen("succeeded"), 1);
	ZVAL_STR(&enum_case_Succeeded_value, enum_case_Succeeded_value_str);
	zend_enum_add_case_cstr(class_entry, "Succeeded", &enum_case_Succeeded_value);

	zval enum_case_Failed_value;
	zend_string *enum_case_Failed_value_str = zend_string_init("failed", strlen("failed"), 1);
	ZVAL_STR(&enum_case_Failed_value, enum_case_Failed_value_str);
	zend_enum_add_case_cstr(class_entry, "Failed", &enum_case_Failed_value);

	zval enum_case_Interrupted_value;
	zend_string *enum_case_Interrupted_value_str = zend_string_init("interrupted", strlen("interrupted"), 1);
	ZVAL_STR(&enum_case_Interrupted_value, enum_case_Interrupted_value_str);
	zend_enum_add_case_cstr(class_entry, "Interrupted", &enum_case_Interrupted_value);

	zval enum_case_Cancelled_value;
	zend_string *enum_case_Cancelled_value_str = zend_string_init("cancelled", strlen("cancelled"), 1);
	ZVAL_STR(&enum_case_Cancelled_value, enum_case_Cancelled_value_str);
	zend_enum_add_case_cstr(class_entry, "Cancelled", &enum_case_Cancelled_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_RetryMode(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\RetryMode", IS_STRING, NULL);

	zval enum_case_Blocking_value;
	zend_string *enum_case_Blocking_value_str = zend_string_init("blocking", strlen("blocking"), 1);
	ZVAL_STR(&enum_case_Blocking_value, enum_case_Blocking_value_str);
	zend_enum_add_case_cstr(class_entry, "Blocking", &enum_case_Blocking_value);

	zval enum_case_Reschedule_value;
	zend_string *enum_case_Reschedule_value_str = zend_string_init("reschedule", strlen("reschedule"), 1);
	ZVAL_STR(&enum_case_Reschedule_value, enum_case_Reschedule_value_str);
	zend_enum_add_case_cstr(class_entry, "Reschedule", &enum_case_Reschedule_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_RecoveryPolicy(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\RecoveryPolicy", IS_STRING, NULL);

	zval enum_case_FailInterrupted_value;
	zend_string *enum_case_FailInterrupted_value_str = zend_string_init("fail_interrupted", strlen("fail_interrupted"), 1);
	ZVAL_STR(&enum_case_FailInterrupted_value, enum_case_FailInterrupted_value_str);
	zend_enum_add_case_cstr(class_entry, "FailInterrupted", &enum_case_FailInterrupted_value);

	zval enum_case_RetryInterrupted_value;
	zend_string *enum_case_RetryInterrupted_value_str = zend_string_init("retry_interrupted", strlen("retry_interrupted"), 1);
	ZVAL_STR(&enum_case_RetryInterrupted_value, enum_case_RetryInterrupted_value_str);
	zend_enum_add_case_cstr(class_entry, "RetryInterrupted", &enum_case_RetryInterrupted_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_JobType(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\JobType", IS_STRING, NULL);

	zval enum_case_Cli_value;
	zend_string *enum_case_Cli_value_str = zend_string_init("cli", strlen("cli"), 1);
	ZVAL_STR(&enum_case_Cli_value, enum_case_Cli_value_str);
	zend_enum_add_case_cstr(class_entry, "Cli", &enum_case_Cli_value);

	zval enum_case_Http_value;
	zend_string *enum_case_Http_value_str = zend_string_init("http", strlen("http"), 1);
	ZVAL_STR(&enum_case_Http_value, enum_case_Http_value_str);
	zend_enum_add_case_cstr(class_entry, "Http", &enum_case_Http_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_OutputChannel(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\OutputChannel", IS_STRING, NULL);

	zval enum_case_Stdout_value;
	zend_string *enum_case_Stdout_value_str = zend_string_init("stdout", strlen("stdout"), 1);
	ZVAL_STR(&enum_case_Stdout_value, enum_case_Stdout_value_str);
	zend_enum_add_case_cstr(class_entry, "Stdout", &enum_case_Stdout_value);

	zval enum_case_Stderr_value;
	zend_string *enum_case_Stderr_value_str = zend_string_init("stderr", strlen("stderr"), 1);
	ZVAL_STR(&enum_case_Stderr_value, enum_case_Stderr_value_str);
	zend_enum_add_case_cstr(class_entry, "Stderr", &enum_case_Stderr_value);

	zval enum_case_Body_value;
	zend_string *enum_case_Body_value_str = zend_string_init("body", strlen("body"), 1);
	ZVAL_STR(&enum_case_Body_value, enum_case_Body_value_str);
	zend_enum_add_case_cstr(class_entry, "Body", &enum_case_Body_value);

	zval enum_case_Headers_value;
	zend_string *enum_case_Headers_value_str = zend_string_init("headers", strlen("headers"), 1);
	ZVAL_STR(&enum_case_Headers_value, enum_case_Headers_value_str);
	zend_enum_add_case_cstr(class_entry, "Headers", &enum_case_Headers_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_Exception(zend_class_entry *class_entry_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Exception", class_JobU_Exception_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	zval property__errorCode_default_value;
	ZVAL_NULL(&property__errorCode_default_value);
	zend_string *property__errorCode_name = zend_string_init("_errorCode", sizeof("_errorCode") - 1, 1);
	zend_declare_typed_property(class_entry, property__errorCode_name, &property__errorCode_default_value, ZEND_ACC_PRIVATE, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release(property__errorCode_name);

	zval property__category_default_value;
	ZVAL_NULL(&property__category_default_value);
	zend_string *property__category_name = zend_string_init("_category", sizeof("_category") - 1, 1);
	zend_declare_typed_property(class_entry, property__category_name, &property__category_default_value, ZEND_ACC_PRIVATE, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release(property__category_name);

	return class_entry;
}

static zend_class_entry *register_class_JobU_ConnectionException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "ConnectionException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_TimeoutException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "TimeoutException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_NotFoundException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "NotFoundException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_RevisionConflictException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "RevisionConflictException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_InvalidCursorException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "InvalidCursorException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_UnsavedChangesException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "UnsavedChangesException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_OutcomeUnknownException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "OutcomeUnknownException", class_JobU_OutcomeUnknownException_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	zval property__operation_default_value;
	ZVAL_NULL(&property__operation_default_value);
	zend_string *property__operation_name = zend_string_init("_operation", sizeof("_operation") - 1, 1);
	zend_declare_typed_property(class_entry, property__operation_name, &property__operation_default_value, ZEND_ACC_PRIVATE, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release(property__operation_name);

	zval property__idempotencyKey_default_value;
	ZVAL_NULL(&property__idempotencyKey_default_value);
	zend_string *property__idempotencyKey_name = zend_string_init("_idempotencyKey", sizeof("_idempotencyKey") - 1, 1);
	zend_declare_typed_property(class_entry, property__idempotencyKey_name, &property__idempotencyKey_default_value, ZEND_ACC_PRIVATE, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_STRING|MAY_BE_NULL));
	zend_string_release(property__idempotencyKey_name);

	return class_entry;
}

static zend_class_entry *register_class_JobU_UnsupportedFeatureException(zend_class_entry *class_entry_JobU_Exception)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "UnsupportedFeatureException", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_Exception, ZEND_ACC_NOT_SERIALIZABLE);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_Exception);
	class_entry->ce_flags |= ZEND_ACC_NOT_SERIALIZABLE;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_TimeRange(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "TimeRange", class_JobU_TimeRange_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_RunQuery(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "RunQuery", class_JobU_RunQuery_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_StatisticsQuery(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "StatisticsQuery", class_JobU_StatisticsQuery_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_SystemInfo(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "SystemInfo", class_JobU_SystemInfo_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_SecretMetadata(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "SecretMetadata", class_JobU_SecretMetadata_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_ExecutionResult(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "ExecutionResult", class_JobU_ExecutionResult_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_ABSTRACT);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_ABSTRACT;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_CliExecutionResult(zend_class_entry *class_entry_JobU_ExecutionResult)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "CliExecutionResult", class_JobU_CliExecutionResult_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_ExecutionResult, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_ExecutionResult);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_HttpExecutionResult(zend_class_entry *class_entry_JobU_ExecutionResult)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "HttpExecutionResult", class_JobU_HttpExecutionResult_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, class_entry_JobU_ExecutionResult, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, class_entry_JobU_ExecutionResult);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_OutputChunk(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "OutputChunk", class_JobU_OutputChunk_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_RunOrigin(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\RunOrigin", IS_STRING, NULL);

	zval enum_case_Scheduled_value;
	zend_string *enum_case_Scheduled_value_str = zend_string_init("scheduled", strlen("scheduled"), 1);
	ZVAL_STR(&enum_case_Scheduled_value, enum_case_Scheduled_value_str);
	zend_enum_add_case_cstr(class_entry, "Scheduled", &enum_case_Scheduled_value);

	zval enum_case_Manual_value;
	zend_string *enum_case_Manual_value_str = zend_string_init("manual", strlen("manual"), 1);
	ZVAL_STR(&enum_case_Manual_value, enum_case_Manual_value_str);
	zend_enum_add_case_cstr(class_entry, "Manual", &enum_case_Manual_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_StatisticsGroupBy(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\StatisticsGroupBy", IS_STRING, NULL);

	zval enum_case_None_value;
	zend_string *enum_case_None_value_str = zend_string_init("none", strlen("none"), 1);
	ZVAL_STR(&enum_case_None_value, enum_case_None_value_str);
	zend_enum_add_case_cstr(class_entry, "None", &enum_case_None_value);

	zval enum_case_Queue_value;
	zend_string *enum_case_Queue_value_str = zend_string_init("queue", strlen("queue"), 1);
	ZVAL_STR(&enum_case_Queue_value, enum_case_Queue_value_str);
	zend_enum_add_case_cstr(class_entry, "Queue", &enum_case_Queue_value);

	zval enum_case_Job_value;
	zend_string *enum_case_Job_value_str = zend_string_init("job", strlen("job"), 1);
	ZVAL_STR(&enum_case_Job_value, enum_case_Job_value_str);
	zend_enum_add_case_cstr(class_entry, "Job", &enum_case_Job_value);

	zval enum_case_Type_value;
	zend_string *enum_case_Type_value_str = zend_string_init("type", strlen("type"), 1);
	ZVAL_STR(&enum_case_Type_value, enum_case_Type_value_str);
	zend_enum_add_case_cstr(class_entry, "Type", &enum_case_Type_value);

	zval enum_case_Origin_value;
	zend_string *enum_case_Origin_value_str = zend_string_init("origin", strlen("origin"), 1);
	ZVAL_STR(&enum_case_Origin_value, enum_case_Origin_value_str);
	zend_enum_add_case_cstr(class_entry, "Origin", &enum_case_Origin_value);

	zval enum_case_State_value;
	zend_string *enum_case_State_value_str = zend_string_init("state", strlen("state"), 1);
	ZVAL_STR(&enum_case_State_value, enum_case_State_value_str);
	zend_enum_add_case_cstr(class_entry, "State", &enum_case_State_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_OutputStatus(void)
{
	zend_class_entry *class_entry = zend_register_internal_enum("JobU\\OutputStatus", IS_STRING, NULL);

	zval enum_case_Available_value;
	zend_string *enum_case_Available_value_str = zend_string_init("available", strlen("available"), 1);
	ZVAL_STR(&enum_case_Available_value, enum_case_Available_value_str);
	zend_enum_add_case_cstr(class_entry, "Available", &enum_case_Available_value);

	zval enum_case_Pending_value;
	zend_string *enum_case_Pending_value_str = zend_string_init("pending", strlen("pending"), 1);
	ZVAL_STR(&enum_case_Pending_value, enum_case_Pending_value_str);
	zend_enum_add_case_cstr(class_entry, "Pending", &enum_case_Pending_value);

	zval enum_case_NotCaptured_value;
	zend_string *enum_case_NotCaptured_value_str = zend_string_init("not_captured", strlen("not_captured"), 1);
	ZVAL_STR(&enum_case_NotCaptured_value, enum_case_NotCaptured_value_str);
	zend_enum_add_case_cstr(class_entry, "NotCaptured", &enum_case_NotCaptured_value);

	zval enum_case_Lost_value;
	zend_string *enum_case_Lost_value_str = zend_string_init("lost", strlen("lost"), 1);
	ZVAL_STR(&enum_case_Lost_value, enum_case_Lost_value_str);
	zend_enum_add_case_cstr(class_entry, "Lost", &enum_case_Lost_value);

	return class_entry;
}

static zend_class_entry *register_class_JobU_Job(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Job", class_JobU_Job_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_Run(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Run", class_JobU_Run_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobU_Page(void)
{
	zend_class_entry ce, *class_entry;

	INIT_NS_CLASS_ENTRY(ce, "JobU", "Page", class_JobU_Page_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}
