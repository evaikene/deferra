/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: bf1e258c8986dabbfbed8bc26721726ec3c7d65e */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_JobUTestResource___construct, 0, 0, 2)
	ZEND_ARG_OBJ_INFO(0, client, JobU\\Client, 0)
	ZEND_ARG_TYPE_INFO(0, id, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestResource_name, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestResource_revision, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestResource_dirty, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_class_JobUTestResource_defaults, 0, 0, JobU\\QueueDefaults, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobUTestResource_notifications arginfo_class_JobUTestResource_revision

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestResource_setName, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestResource_save, 0, 0, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, timeout, JobU\\Duration, 1, "null")
ZEND_END_ARG_INFO()

#define arginfo_class_JobUTestResource_refresh arginfo_class_JobUTestResource_save

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_JobUTestResource_discardChanges, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

#define arginfo_class_JobUTestResource_lifecycle arginfo_class_JobUTestResource_discardChanges

#define arginfo_class_JobUTestResource_markDeleted arginfo_class_JobUTestResource_discardChanges

#define arginfo_class_JobUTestResource_rejectNextSnapshot arginfo_class_JobUTestResource_discardChanges

ZEND_METHOD(JobUTestResource, __construct);
ZEND_METHOD(JobUTestResource, name);
ZEND_METHOD(JobUTestResource, revision);
ZEND_METHOD(JobUTestResource, dirty);
ZEND_METHOD(JobUTestResource, defaults);
ZEND_METHOD(JobUTestResource, notifications);
ZEND_METHOD(JobUTestResource, setName);
ZEND_METHOD(JobUTestResource, save);
ZEND_METHOD(JobUTestResource, refresh);
ZEND_METHOD(JobUTestResource, discardChanges);
ZEND_METHOD(JobUTestResource, lifecycle);
ZEND_METHOD(JobUTestResource, markDeleted);
ZEND_METHOD(JobUTestResource, rejectNextSnapshot);

static const zend_function_entry class_JobUTestResource_methods[] = {
	ZEND_ME(JobUTestResource, __construct, arginfo_class_JobUTestResource___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, name, arginfo_class_JobUTestResource_name, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, revision, arginfo_class_JobUTestResource_revision, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, dirty, arginfo_class_JobUTestResource_dirty, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, defaults, arginfo_class_JobUTestResource_defaults, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, notifications, arginfo_class_JobUTestResource_notifications, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, setName, arginfo_class_JobUTestResource_setName, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, save, arginfo_class_JobUTestResource_save, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, refresh, arginfo_class_JobUTestResource_refresh, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, discardChanges, arginfo_class_JobUTestResource_discardChanges, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, lifecycle, arginfo_class_JobUTestResource_lifecycle, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, markDeleted, arginfo_class_JobUTestResource_markDeleted, ZEND_ACC_PUBLIC)
	ZEND_ME(JobUTestResource, rejectNextSnapshot, arginfo_class_JobUTestResource_rejectNextSnapshot, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_JobUTestResource(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "JobUTestResource", class_JobUTestResource_methods);
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
