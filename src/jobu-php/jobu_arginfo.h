/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: a86314e5638a7012b34537a1e6871c547aab092f */

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

#define arginfo_class_JobU_Exception_getErrorCode arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_Exception_getCategory arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_OutcomeUnknownException_getOperation arginfo_class_JobU_JobAttributes_getOutputCapture

#define arginfo_class_JobU_OutcomeUnknownException_getIdempotencyKey arginfo_class_JobU_JobAttributes_getOutputCapture

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
ZEND_METHOD(JobU_Exception, getErrorCode);
ZEND_METHOD(JobU_Exception, getCategory);
ZEND_METHOD(JobU_OutcomeUnknownException, getOperation);
ZEND_METHOD(JobU_OutcomeUnknownException, getIdempotencyKey);

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
