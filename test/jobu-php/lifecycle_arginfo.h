/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 02be6394a8230d2fdc5cba8b0a988118ac845a22 */

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

ZEND_METHOD(JobUTestClient, __construct);
ZEND_METHOD(JobUTestClient, close);
ZEND_METHOD(JobUTestClient, ping);
ZEND_METHOD(JobUTestClient, pending);
ZEND_METHOD(JobUTestClient, live);
ZEND_METHOD(JobUTestHolder, __construct);
ZEND_METHOD(JobUTestHolder, client);

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
