/* This is a generated file, edit the .stub.php file instead.
 * Stub hash: a8549db466e25000e63af259174b0b36ecc45236 */

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_PhpCxxNode___construct, 0, 0, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, number, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, reference, IS_MIXED, 0, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, fail, _IS_BOOL, 0, "false")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PhpCxxNode_number, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PhpCxxNode_setNumber, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, number, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PhpCxxNode_reference, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PhpCxxNode_setReference, 0, 1, IS_VOID, 0)
	ZEND_ARG_TYPE_INFO(0, reference, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PhpCxxNode_accepts, 0, 1, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, object, IS_OBJECT, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_class_PhpCxxNode_counts, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_class_PhpCxxLocked___construct, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_METHOD(PhpCxxNode, __construct);
ZEND_METHOD(PhpCxxNode, number);
ZEND_METHOD(PhpCxxNode, setNumber);
ZEND_METHOD(PhpCxxNode, reference);
ZEND_METHOD(PhpCxxNode, setReference);
ZEND_METHOD(PhpCxxNode, accepts);
ZEND_METHOD(PhpCxxNode, counts);
ZEND_METHOD(PhpCxxLocked, __construct);

static const zend_function_entry class_PhpCxxNode_methods[] = {
	ZEND_ME(PhpCxxNode, __construct, arginfo_class_PhpCxxNode___construct, ZEND_ACC_PUBLIC)
	ZEND_ME(PhpCxxNode, number, arginfo_class_PhpCxxNode_number, ZEND_ACC_PUBLIC)
	ZEND_ME(PhpCxxNode, setNumber, arginfo_class_PhpCxxNode_setNumber, ZEND_ACC_PUBLIC)
	ZEND_ME(PhpCxxNode, reference, arginfo_class_PhpCxxNode_reference, ZEND_ACC_PUBLIC)
	ZEND_ME(PhpCxxNode, setReference, arginfo_class_PhpCxxNode_setReference, ZEND_ACC_PUBLIC)
	ZEND_ME(PhpCxxNode, accepts, arginfo_class_PhpCxxNode_accepts, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_ME(PhpCxxNode, counts, arginfo_class_PhpCxxNode_counts, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	ZEND_FE_END
};

static const zend_function_entry class_PhpCxxLocked_methods[] = {
	ZEND_ME(PhpCxxLocked, __construct, arginfo_class_PhpCxxLocked___construct, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};

static zend_class_entry *register_class_PhpCxxNode(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "PhpCxxNode", class_PhpCxxNode_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, 0);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
#endif

	zval property_property_default_value;
	ZVAL_NULL(&property_property_default_value);
	zend_string *property_property_name = zend_string_init("property", sizeof("property") - 1, 1);
	zend_declare_typed_property(class_entry, property_property_name, &property_property_default_value, ZEND_ACC_PUBLIC, NULL, (zend_type) ZEND_TYPE_INIT_MASK(MAY_BE_ANY));
	zend_string_release(property_property_name);

	return class_entry;
}

static zend_class_entry *register_class_PhpCxxLocked(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "PhpCxxLocked", class_PhpCxxLocked_methods);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}

static zend_class_entry *register_class_PhpCxxImmobile(void)
{
	zend_class_entry ce, *class_entry;

	INIT_CLASS_ENTRY(ce, "PhpCxxImmobile", NULL);
#if (PHP_VERSION_ID >= 80400)
	class_entry = zend_register_internal_class_with_flags(&ce, NULL, ZEND_ACC_FINAL);
#else
	class_entry = zend_register_internal_class_ex(&ce, NULL);
	class_entry->ce_flags |= ZEND_ACC_FINAL;
#endif

	return class_entry;
}
