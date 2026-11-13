/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: 037a7e0dc3a56c74923f1f6c28847ec6f0ba899b */

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobUTestPages_create, 0, 2, JobU\\Page, 0)
	ZEND_ARG_OBJ_INFO(0, client, JobU\\Client, 0)
	ZEND_ARG_TYPE_INFO(0, kind, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "1")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, id, IS_STRING, 1, "null")
ZEND_END_ARG_INFO()

ZEND_METHOD(JobUTestPages, create);

static const zend_function_entry class_JobUTestPages_methods[] = {
	ZEND_ME(JobUTestPages, create, arginfo_class_JobUTestPages_create, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_JobUTestPages(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "JobUTestPages", class_JobUTestPages_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}
