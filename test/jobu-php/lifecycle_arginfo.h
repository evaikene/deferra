/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 19255a78581528ac3bdbd8e8d1d6acf80ccc8f4e */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobUTestClient___construct, 0, 0, 1)
	ZEND_ARG_TYPE_INFO(0, socket, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fail, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestClient_close, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestClient_ping, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobUTestClient_pending arginfo_class_JobUTestClient_close

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestClient_live, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobUTestHolder___construct, 0, 0, 1)
	ZEND_ARG_OBJ_INFO(0, client, JobUTestClient, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobUTestHolder_client, 0, 0, JobUTestClient, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestBridge_ping, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_OBJ_INFO(0, client, JobU\\Client, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestBridge_createQueue, 0, 2, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO(0, client, JobU\\Client, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, key, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestBridge_failure, 0, 3, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, code, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, category, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, remote, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, unknown, _IS_BOOL, 0, "false")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, key, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, pending, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestBridge_timeout, 0, 1, IS_LONG, 0)
	ZEND_ARG_OBJ_INFO(0, client, JobU\\Client, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(JobUTestClient, __construct);
ZEND_METHOD(JobUTestClient, close);
ZEND_METHOD(JobUTestClient, ping);
ZEND_METHOD(JobUTestClient, pending);
ZEND_METHOD(JobUTestClient, live);
ZEND_METHOD(JobUTestHolder, __construct);
ZEND_METHOD(JobUTestHolder, client);
ZEND_METHOD(JobUTestBridge, ping);
ZEND_METHOD(JobUTestBridge, createQueue);
ZEND_METHOD(JobUTestBridge, failure);
ZEND_METHOD(JobUTestBridge, timeout);

static const zend_function_entry class_JobUTestClient_methods[] = {
	ZEND_ME(JobUTestClient, __construct, arginfo_class_JobUTestClient___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestClient, close, arginfo_class_JobUTestClient_close, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestClient, ping, arginfo_class_JobUTestClient_ping, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestClient, pending, arginfo_class_JobUTestClient_pending, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestClient, live, arginfo_class_JobUTestClient_live, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobUTestHolder_methods[] = {
	ZEND_ME(JobUTestHolder, __construct, arginfo_class_JobUTestHolder___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestHolder, client, arginfo_class_JobUTestHolder_client, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static const zend_function_entry class_JobUTestBridge_methods[] = {
	ZEND_ME(JobUTestBridge, ping, arginfo_class_JobUTestBridge_ping, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobUTestBridge, createQueue, arginfo_class_JobUTestBridge_createQueue, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobUTestBridge, failure, arginfo_class_JobUTestBridge_failure, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(JobUTestBridge, timeout, arginfo_class_JobUTestBridge_timeout, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_JobUTestClient(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "JobUTestClient", class_JobUTestClient_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	zval property_reference_default_value;
	ZVAL_NULL(&property_reference_default_value);
	zend_string *property_reference_name = zend_string_init("reference", sizeof("reference") - 1, 1);
	zend_declare_typed_property(class_entry, property_reference_name, &property_reference_default_value, ZEND_ACC_PUBLIC, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_ANY));
	zend_string_release(property_reference_name);

	return class_entry;
}

static zend_class_entry *register_class_JobUTestHolder(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "JobUTestHolder", class_JobUTestHolder_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_JobUTestBridge(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "JobUTestBridge", class_JobUTestBridge_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}
