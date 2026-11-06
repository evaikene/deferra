/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 12722f41e4cf24cea606ff6ee45720454d239836 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_Duration___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobU_Duration_milliseconds, 0, 1, JobU\\Duration, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Duration_seconds arginfo_class_JobU_Duration_milliseconds

#define arginfo_class_JobU_Duration_minutes arginfo_class_JobU_Duration_milliseconds

#define arginfo_class_JobU_Duration_hours arginfo_class_JobU_Duration_milliseconds

#define arginfo_class_JobU_Duration_days arginfo_class_JobU_Duration_milliseconds

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobU_Client___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, socketPath, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Client_close, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobU_Exception_getErrorCode, 0, 0, IS_STRING, 1)
ZEND_END_ARG_INFO()

#define arginfo_class_JobU_Exception_getCategory arginfo_class_JobU_Exception_getErrorCode

#define arginfo_class_JobU_OutcomeUnknownException_getOperation arginfo_class_JobU_Exception_getErrorCode

#define arginfo_class_JobU_OutcomeUnknownException_getIdempotencyKey arginfo_class_JobU_Exception_getErrorCode

ZEND_METHOD(JobU_Duration, __construct);
ZEND_METHOD(JobU_Duration, milliseconds);
ZEND_METHOD(JobU_Duration, seconds);
ZEND_METHOD(JobU_Duration, minutes);
ZEND_METHOD(JobU_Duration, hours);
ZEND_METHOD(JobU_Duration, days);
ZEND_METHOD(JobU_Client, __construct);
ZEND_METHOD(JobU_Client, close);
ZEND_METHOD(JobU_Exception, getErrorCode);
ZEND_METHOD(JobU_Exception, getCategory);
ZEND_METHOD(JobU_OutcomeUnknownException, getOperation);
ZEND_METHOD(JobU_OutcomeUnknownException, getIdempotencyKey);

static const zend_function_entry class_JobU_Duration_methods[] = {
	ZEND_ME(JobU_Duration, __construct, arginfo_class_JobU_Duration___construct, ZEND_ACC_PRIVATE)
	ZEND_ME(JobU_Duration, milliseconds, arginfo_class_JobU_Duration_milliseconds, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, seconds, arginfo_class_JobU_Duration_seconds, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, minutes, arginfo_class_JobU_Duration_minutes, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, hours, arginfo_class_JobU_Duration_hours, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobU_Duration, days, arginfo_class_JobU_Duration_days, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
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
