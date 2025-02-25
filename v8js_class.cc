/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2017 The PHP Group                                |
  +----------------------------------------------------------------------+
  | http://www.opensource.org/licenses/mit-license.php  MIT License      |
  +----------------------------------------------------------------------+
  | Author: Jani Taskinen <jani.taskinen@iki.fi>                         |
  | Author: Patrick Reilly <preilly@php.net>                             |
  | Author: Stefan Siegl <stesie@php.net>                                |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <functional>
#include <algorithm>

#include "php_v8js_macros.h"
#include "v8js_v8.h"
#include "v8js_exceptions.h"
#include "v8js_v8object_class.h"
#include "v8js_object_export.h"
#include "v8js_timer.h"

extern "C" {
#include "php.h"
#include "ext/date/php_date.h"
#include "ext/standard/php_string.h"
#include "zend_interfaces.h"
#include "zend_closures.h"
#include "ext/spl/spl_exceptions.h"
#include "zend_exceptions.h"
}

#define PHP_V8JS_SCRIPT_RES_NAME "V8Js script"

/* {{{ Class Entries */
static zend_class_entry *php_ce_v8js;
/* }}} */

/* {{{ Object Handlers */
static zend_object_handlers v8js_object_handlers;
/* }}} */

/* Forward declare v8js_methods, actually "static" but not possible in C++ */
extern const zend_function_entry v8js_methods[];

typedef struct _v8js_script {
	char *name;
	v8js_ctx *ctx;
	v8::Persistent<v8::Script, v8::CopyablePersistentTraits<v8::Script>> *script;
} v8js_script;

static void v8js_script_free(v8js_script *res);

int le_v8js_script;

#ifdef USE_INTERNAL_ALLOCATOR
class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
public:
	virtual void* Allocate(size_t length) {
		void* data = AllocateUninitialized(length);
		return data == NULL ? data : memset(data, 0, length);
	}
	virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
	virtual void Free(void* data, size_t) { free(data); }
};
#endif  /** USE_INTERNAL_ALLOCATOR */


static void v8js_free_storage(zend_object *object) /* {{{ */
{
	v8js_ctx *c = v8js_ctx_fetch_object(object);

	zend_object_std_dtor(&c->std);

	zval_ptr_dtor(&c->module_normaliser);
	zval_ptr_dtor(&c->module_loader);

	/* Delete PHP global object from JavaScript */
	if (!c->context.IsEmpty()) {
		v8::Locker locker(c->isolate);
		v8::Isolate::Scope isolate_scope(c->isolate);
		v8::HandleScope handle_scope(c->isolate);
		v8::Local<v8::Context> v8_context = v8::Local<v8::Context>::New(c->isolate, c->context);
		v8::Context::Scope context_scope(v8_context);
		v8::Local<v8::String> object_name_js = v8::Local<v8::String>::New(c->isolate, c->object_name);
		V8JS_GLOBAL(c->isolate)->Delete(v8_context, object_name_js);
	}

	c->object_name.Reset();
	c->object_name.~Persistent();
	c->global_template.Reset();
	c->global_template.~Persistent();
	c->array_tmpl.Reset();
	c->array_tmpl.~Persistent();

	/* Clear persistent call_impl & method_tmpls templates */
	for (std::map<v8js_function_tmpl_t *, v8js_function_tmpl_t>::iterator it = c->call_impls.begin();
		 it != c->call_impls.end(); ++it) {
		// No need to free it->first, as it is stored in c->template_cache and freed below
		it->second.Reset();
	}
	c->call_impls.~map();

	for (std::map<std::pair<zend_class_entry *, zend_function *>, v8js_function_tmpl_t>::iterator it = c->method_tmpls.begin();
		 it != c->method_tmpls.end(); ++it) {
		it->second.Reset();
	}
	c->method_tmpls.~map();

	/* Clear persistent handles in template cache */
	for (std::map<const zend_string *,v8js_function_tmpl_t>::iterator it = c->template_cache.begin();
		 it != c->template_cache.end(); ++it) {
		it->second.Reset();
	}
	c->template_cache.~map();

	/* Clear contexts */
	for (std::vector<v8js_accessor_ctx*>::iterator it = c->accessor_list.begin();
		 it != c->accessor_list.end(); ++it) {
		v8js_accessor_ctx_dtor(*it);
	}
	c->accessor_list.~vector();

	/* Clear global object, dispose context */
	if (!c->context.IsEmpty()) {
		c->context.Reset();
	}
	c->context.~Persistent();

	/* Dispose yet undisposed weak refs */
	for (std::map<zend_object *, v8js_persistent_obj_t>::iterator it = c->weak_objects.begin();
		 it != c->weak_objects.end(); ++it) {
		zend_object *object = it->first;
		zval value;
		ZVAL_OBJ(&value, object);
		zval_ptr_dtor(&value);
		c->isolate->AdjustAmountOfExternalAllocatedMemory(-c->average_object_size);
		it->second.Reset();
	}
	c->weak_objects.~map();

	for (std::map<v8js_function_tmpl_t *, v8js_persistent_obj_t>::iterator it = c->weak_closures.begin();
		 it != c->weak_closures.end(); ++it) {
		v8js_function_tmpl_t *persist_tpl_ = it->first;
		persist_tpl_->Reset();
		delete persist_tpl_;
		it->second.Reset();
	}
	c->weak_closures.~map();

	for (std::list<v8js_v8object *>::iterator it = c->v8js_v8objects.begin();
		 it != c->v8js_v8objects.end(); it ++) {
		(*it)->v8obj.Reset();
		(*it)->ctx = NULL;
	}
	c->v8js_v8objects.~list();

	for (std::vector<v8js_script *>::iterator it = c->script_objects.begin();
		 it != c->script_objects.end(); it ++) {
		(*it)->ctx = NULL;
		(*it)->script->Reset();
	}
	c->script_objects.~vector();

	/* Clear persistent handles in module cache */
	for (std::map<char *, v8js_persistent_value_t>::iterator it = c->modules_loaded.begin();
		 it != c->modules_loaded.end(); ++it) {
		efree(it->first);
		it->second.Reset();
	}
	c->modules_loaded.~map();

	if(c->isolate) {
		/* c->isolate is initialized by V8Js::__construct, but __wakeup calls
		 * are not fully constructed and hence this would cause a NPE. */
		c->isolate->Dispose();
	}

	if(c->tz != NULL) {
		free(c->tz);
	}

	c->modules_stack.~vector();

	zval_ptr_dtor(&c->zval_snapshot_blob);

#ifndef USE_INTERNAL_ALLOCATOR
	delete c->create_params.array_buffer_allocator;
#endif
}
/* }}} */

static zend_object* v8js_new(zend_class_entry *ce) /* {{{ */
{
	v8js_ctx *c;

	c = (v8js_ctx *) ecalloc(1, sizeof(*c) + zend_object_properties_size(ce));
	zend_object_std_init(&c->std, ce);
	object_properties_init(&c->std, ce);

	c->std.handlers = &v8js_object_handlers;

	new(&c->object_name) v8::Persistent<v8::String>();
	new(&c->context) v8::Persistent<v8::Context>();
	new(&c->global_template) v8::Persistent<v8::FunctionTemplate>();
	new(&c->array_tmpl) v8::Persistent<v8::FunctionTemplate>();

	new(&c->modules_stack) std::vector<char*>();
	new(&c->modules_loaded) std::map<char *, v8js_persistent_value_t, cmp_str>;

	new(&c->template_cache) std::map<const zend_string *,v8js_function_tmpl_t>();
	new(&c->accessor_list) std::vector<v8js_accessor_ctx *>();

	new(&c->weak_closures) std::map<v8js_function_tmpl_t *, v8js_persistent_obj_t>();
	new(&c->weak_objects) std::map<zend_object *, v8js_persistent_obj_t>();
	new(&c->call_impls) std::map<v8js_function_tmpl_t *, v8js_function_tmpl_t>();
	new(&c->method_tmpls) std::map<std::pair<zend_class_entry *, zend_function *>, v8js_function_tmpl_t>();

	new(&c->v8js_v8objects) std::list<v8js_v8object *>();
	new(&c->script_objects) std::vector<v8js_script *>();

	// @fixme following is const, run on startup
	v8js_object_handlers.offset = XtOffsetOf(struct v8js_ctx, std);
	v8js_object_handlers.free_obj = v8js_free_storage;

	c->average_object_size = 1024;

	return &c->std;
}
/* }}} */

static void v8js_fatal_error_handler(const char *location, const char *message) /* {{{ */
{
	if (location) {
		zend_error(E_WARNING, "Fatal V8 error in %s: %s", location, message);
	} else {
		zend_error(E_WARNING, "Fatal V8 error: %s", message);
	}
}
/* }}} */

#define IS_MAGIC_FUNC(mname) \
	((ZSTR_LEN(key) == sizeof(mname) - 1) &&		\
	 !strncasecmp(ZSTR_VAL(key), mname, ZSTR_LEN(key)))

/* {{{ proto void V8Js::__construct([string object_name [, array variables [, string snapshot_blob]]])
   __construct for V8Js */
static PHP_METHOD(V8Js, __construct)
{
	zend_string *object_name = NULL;
	zval *vars_arr = NULL;
	zval *snapshot_blob = NULL;

	v8js_ctx *c = Z_V8JS_CTX_OBJ_P(getThis())

	if (!c->context.IsEmpty()) {
		/* called __construct() twice, bail out */
		return;
	}

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|S!az", &object_name, &vars_arr, &snapshot_blob) == FAILURE) {
		return;
	}

	/* Initialize V8 */
	v8js_v8_init();

	/* Throw PHP exception if uncaught exceptions exist */
	c->in_execution = 0;

	new (&c->create_params) v8::Isolate::CreateParams();

#ifdef USE_INTERNAL_ALLOCATOR
	static ArrayBufferAllocator array_buffer_allocator;
	c->create_params.array_buffer_allocator = &array_buffer_allocator;
#else
	c->create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
#endif

	new (&c->snapshot_blob) v8::StartupData();
	if (snapshot_blob) {
		if (Z_TYPE_P(snapshot_blob) == IS_STRING) {
			ZVAL_COPY(&c->zval_snapshot_blob, snapshot_blob);

			if (Z_STRLEN_P(snapshot_blob) > std::numeric_limits<int>::max()) {
				zend_throw_exception(php_ce_v8js_exception,
					"Snapshot size exceeds maximum supported length", 0);
				return;
			}

			c->snapshot_blob.data = Z_STRVAL_P(snapshot_blob);
			c->snapshot_blob.raw_size = static_cast<int>(Z_STRLEN_P(snapshot_blob));
			c->create_params.snapshot_blob = &c->snapshot_blob;
		} else {
			php_error_docref(NULL, E_WARNING, "Argument snapshot_blob expected to be of string type");
		}
	}

	c->isolate = v8::Isolate::New(c->create_params);
	c->isolate->SetData(0, c);

	c->time_limit = 0;
	c->time_limit_hit = false;
	c->memory_limit = 0;
	c->memory_limit_hit = false;

	ZVAL_NULL(&c->module_normaliser);
	ZVAL_NULL(&c->module_loader);

	// Isolate execution
	v8::Isolate *isolate = c->isolate;
	v8::Locker locker(isolate);
	v8::Isolate::Scope isolate_scope(isolate);

	/* Handle scope */
	v8::HandleScope handle_scope(isolate);

	/* Redirect fatal errors to PHP error handler */
	isolate->SetFatalErrorHandler(v8js_fatal_error_handler);

	/* Create global template for global object */
	// Now we are using multiple isolates this needs to be created for every context

	v8::Local<v8::ObjectTemplate> global_template = v8::ObjectTemplate::New(c->isolate);
	c->global_template.Reset(isolate, global_template);

	/* Register builtin methods */
	v8js_register_methods(global_template, c);

	/* Create context */
	v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global_template);

	if (context.IsEmpty()) {
		zend_throw_exception(php_ce_v8js_exception, "Failed to create V8 context.", 0);
		return;
	}

	context->SetAlignedPointerInEmbedderData(1, c);
	context->Global()->Set(context, V8JS_SYM("global"), context->Global());
	c->context.Reset(isolate, context);

	/* Enter context */
	v8::Context::Scope context_scope(context);

	/* Create the PHP container object's function template */
	v8::Local<v8::FunctionTemplate> php_obj_t = v8::FunctionTemplate::New(isolate, 0);

	/* Set class name for PHP object */
	zend_class_entry *ce = Z_OBJCE_P(getThis());

	if (ZSTR_LEN(ce->name) > std::numeric_limits<int>::max()) {
		zend_throw_exception(php_ce_v8js_exception,
			"PHP object class name exceeds maximum supported length", 0);
		return;
	}

	php_obj_t->SetClassName(V8JS_SYML(ZSTR_VAL(ce->name), static_cast<int>(ZSTR_LEN(ce->name))));

	/* Register Get accessor for passed variables */
	if (vars_arr && zend_hash_num_elements(Z_ARRVAL_P(vars_arr)) > 0) {
		v8js_register_accessors(&c->accessor_list, php_obj_t, vars_arr, isolate);
	}

	/* Set name for the PHP JS object */
	v8::Local<v8::String> object_name_js;

	if (object_name && ZSTR_LEN(object_name)) {
		if (ZSTR_LEN(object_name) > std::numeric_limits<int>::max()) {
			zend_throw_exception(php_ce_v8js_exception,
				"PHP JS object class name exceeds maximum supported length", 0);
			return;
		}

		object_name_js = V8JS_ZSYM(object_name);
	}
	else {
		object_name_js = V8JS_SYM("PHP");
	}

	c->object_name.Reset(isolate, object_name_js);

	/* Add the PHP object into global object */
	php_obj_t->InstanceTemplate()->SetInternalFieldCount(2);
	v8::Local<v8::Object> php_obj = php_obj_t->InstanceTemplate()->NewInstance(context).ToLocalChecked();
	V8JS_GLOBAL(isolate)->DefineOwnProperty(context, object_name_js, php_obj, v8::ReadOnly);

	/* Export public property values */
	HashTable *properties = zend_std_get_properties(Z_OBJ_P(getThis()));
	zval *value;
	zend_string *member;

	ZEND_HASH_FOREACH_STR_KEY(properties, member) {
		zend_property_info *property_info = zend_get_property_info(c->std.ce, member, 1);
		if(property_info &&
		   property_info != ZEND_WRONG_PROPERTY_INFO &&
		   (property_info->flags & ZEND_ACC_PUBLIC)) {
			if (ZSTR_LEN(member) > std::numeric_limits<int>::max()) {
				zend_throw_exception(php_ce_v8js_exception,
					"Property name exceeds maximum supported length", 0);
				return;
			}

			v8::Local<v8::Name> key = V8JS_ZSYM(member);

			/* Write value to PHP JS object */
			value = OBJ_PROP(Z_OBJ_P(getThis()), property_info->offset);
			php_obj->DefineOwnProperty(context, key, zval_to_v8js(value, isolate), v8::ReadOnly);
		}
	} ZEND_HASH_FOREACH_END();

	/* Add pointer to zend object */
	php_obj->SetAlignedPointerInInternalField(1, Z_OBJ_P(getThis()));

	/* Export public methods */
	void *ptr;
	zend_string *key;

	ZEND_HASH_FOREACH_STR_KEY_PTR(&c->std.ce->function_table, key, ptr) {
		zend_function *method_ptr = reinterpret_cast<zend_function *>(ptr);

		if ((method_ptr->common.fn_flags & ZEND_ACC_PUBLIC) == 0) {
			/* Allow only public methods */
			continue;
		}

		if ((method_ptr->common.fn_flags & (ZEND_ACC_CTOR|ZEND_ACC_DTOR)) != 0) {
			/* no __construct, __destruct(), or __clone() functions */
			continue;
		}

		/* hide (do not export) other PHP magic functions */
		if (IS_MAGIC_FUNC(ZEND_CALLSTATIC_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_SLEEP_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_WAKEUP_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_SET_STATE_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_GET_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_SET_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_UNSET_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_CALL_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_INVOKE_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_TOSTRING_FUNC_NAME) ||
			IS_MAGIC_FUNC(ZEND_ISSET_FUNC_NAME)) {
			continue;
		}

		const zend_function_entry *fe;
		for (fe = v8js_methods; fe->fname; fe ++) {
			if (strcmp(fe->fname, ZSTR_VAL(method_ptr->common.function_name)) == 0) {
				break;
			}
		}

		if(fe->fname) {
			/* Method belongs to \V8Js class itself, never export to V8, even if
			 * it is overriden in a derived class. */
			continue;
		}

		if (ZSTR_LEN(method_ptr->common.function_name) > std::numeric_limits<int>::max()) {
			zend_throw_exception(php_ce_v8js_exception,
				"Method name exceeds maximum supported length", 0);
			return;
		}

		v8::Local<v8::String> method_name = V8JS_ZSYM(method_ptr->common.function_name);
		v8::Local<v8::FunctionTemplate> ft;

		ft = v8::FunctionTemplate::New(isolate, v8js_php_callback,
				v8::External::New((isolate), method_ptr));
		// @fixme add/check Signature v8::Signature::New((isolate), tmpl));
		v8js_function_tmpl_t *persistent_ft = &c->method_tmpls[std::make_pair(ce, method_ptr)];
		persistent_ft->Reset(isolate, ft);

		php_obj->CreateDataProperty(context, method_name, ft->GetFunction(context).ToLocalChecked());
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ proto V8JS::__sleep()
 */
PHP_METHOD(V8Js, __sleep)
{
	zend_throw_exception(php_ce_v8js_exception,
		"You cannot serialize or unserialize V8Js instances", 0);
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto V8JS::__wakeup()
 */
PHP_METHOD(V8Js, __wakeup)
{
	zend_throw_exception(php_ce_v8js_exception,
		"You cannot serialize or unserialize V8Js instances", 0);
	RETURN_FALSE;
}
/* }}} */

static void v8js_compile_script(zval *this_ptr, const zend_string *str, const zend_string *identifier, v8js_script **ret)
{
	v8js_script *res = NULL;

	V8JS_BEGIN_CTX(c, this_ptr)

	/* Catch JS exceptions */
	v8::TryCatch try_catch(isolate);

	/* Set script identifier */
	if (identifier && ZSTR_LEN(identifier) > std::numeric_limits<int>::max()) {
		zend_throw_exception(php_ce_v8js_exception,
			"Script identifier exceeds maximum supported length", 0);
		return;
	}

	v8::Local<v8::String> sname = identifier
		? V8JS_ZSTR(identifier)
		: V8JS_SYM("V8Js::compileString()");
	v8::ScriptOrigin origin(isolate, sname);

	if (ZSTR_LEN(str) > std::numeric_limits<int>::max()) {
		zend_throw_exception(php_ce_v8js_exception,
			"Script source exceeds maximum supported length", 0);
		return;
	}

	v8::Local<v8::String> source = V8JS_ZSTR(str);
	v8::MaybeLocal<v8::Script> script = v8::Script::Compile(v8::Local<v8::Context>::New(isolate, c->context), source, &origin);

	/* Compile errors? */
	if (script.IsEmpty()) {
		v8js_throw_script_exception(c->isolate, &try_catch);
		return;
	}
	res = (v8js_script *)emalloc(sizeof(v8js_script));
	res->script = new v8::Persistent<v8::Script, v8::CopyablePersistentTraits<v8::Script>>(c->isolate, script.ToLocalChecked());

	v8::String::Utf8Value _sname(isolate, sname);
	res->name = estrndup(ToCString(_sname), _sname.length());
	res->ctx = c;
	*ret = res;
	return;
}

static void v8js_execute_script(zval *this_ptr, v8js_script *res, long flags, long time_limit, size_t memory_limit, zval **return_value)
{
	v8js_ctx *c = Z_V8JS_CTX_OBJ_P(this_ptr);
	if (res->ctx != c) {
		zend_error(E_WARNING, "Script resource from wrong V8Js object passed");
		ZVAL_BOOL(*return_value, 0);
		return;
	}

	if (!c->in_execution && time_limit == 0) {
		time_limit = c->time_limit;
	}

	if (!c->in_execution && memory_limit == 0) {
		memory_limit = c->memory_limit;
	}

	/* std::function relies on its dtor to be executed, otherwise it leaks
	 * some memory on bailout. */
	{
		std::function< v8::MaybeLocal<v8::Value>(v8::Isolate *) > v8_call = [c, res](v8::Isolate *isolate) {
			v8::Local<v8::Script> script = v8::Local<v8::Script>::New(isolate, *res->script);
			return script->Run(v8::Local<v8::Context>::New(isolate, c->context));
		};

		v8js_v8_call(c, return_value, flags, time_limit, memory_limit, v8_call);
	}

	if(V8JSG(fatal_error_abort)) {
		/* Check for fatal error marker possibly set by v8js_error_handler; just
		 * rethrow the error since we're now out of V8. */
		zend_bailout();
	}
}

/* {{{ proto mixed V8Js::executeString(string script [, string identifier [, int flags]])
 */
static PHP_METHOD(V8Js, executeString)
{
	zend_string *str = NULL, *identifier = NULL;
	long flags = V8JS_FLAG_NONE, time_limit = 0, memory_limit = 0;
	v8js_script *res = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|S!lll", &str, &identifier, &flags, &time_limit, &memory_limit) == FAILURE) {
		return;
	}

	if (memory_limit < 0) {
		zend_throw_exception(php_ce_v8js_exception,
				"memory_limit must not be negative", 0);
		return;
	}

	v8js_compile_script(getThis(), str, identifier, &res);
	if (!res) {
		RETURN_FALSE;
	}

	zend_try {
		v8js_execute_script(getThis(), res, flags, time_limit, static_cast<size_t>(memory_limit), &return_value);
		v8js_script_free(res);
	}
	zend_catch {
		v8js_script_free(res);
		zend_bailout();
	}
	zend_end_try()

	efree(res);
}
/* }}} */


/* {{{ proto mixed V8Js::compileString(string script [, string identifier])
 */
static PHP_METHOD(V8Js, compileString)
{
	zend_string *str = NULL, *identifier = NULL;
	v8js_script *res = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S|S", &str, &identifier) == FAILURE) {
		return;
	}

	v8js_compile_script(getThis(), str, identifier, &res);
	if (res) {
		RETVAL_RES(zend_register_resource(res, le_v8js_script));

		v8js_ctx *ctx;
		ctx = Z_V8JS_CTX_OBJ_P(getThis());
		ctx->script_objects.push_back(res);
	}
}

/* }}} */

/* {{{ proto mixed V8Js::executeScript(resource script [, int flags]])
 */
static PHP_METHOD(V8Js, executeScript)
{
	long flags = V8JS_FLAG_NONE, time_limit = 0, memory_limit = 0;
	zval *zscript;
	v8js_script *res;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "r|lll", &zscript, &flags, &time_limit, &memory_limit) == FAILURE) {
		return;
	}

	if (memory_limit < 0) {
		zend_throw_exception(php_ce_v8js_exception,
				"memory_limit must not be negative", 0);
		return;
	}

	if((res = (v8js_script *)zend_fetch_resource(Z_RES_P(zscript), PHP_V8JS_SCRIPT_RES_NAME, le_v8js_script)) == NULL) {
		RETURN_FALSE;
	}

	v8js_execute_script(getThis(), res, flags, time_limit, static_cast<size_t>(memory_limit), &return_value);
}
/* }}} */

/* {{{ proto void V8Js::setModuleNormaliser(string base, string module_id)
 */
static PHP_METHOD(V8Js, setModuleNormaliser)
{
	v8js_ctx *c;
	zval *callable;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &callable) == FAILURE) {
		return;
	}

	c = Z_V8JS_CTX_OBJ_P(getThis());
	ZVAL_COPY(&c->module_normaliser, callable);
}
/* }}} */

/* {{{ proto void V8Js::setModuleLoader(string module)
 */
static PHP_METHOD(V8Js, setModuleLoader)
{
	v8js_ctx *c;
	zval *callable;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "z", &callable) == FAILURE) {
		return;
	}

	c = Z_V8JS_CTX_OBJ_P(getThis());
	ZVAL_COPY(&c->module_loader, callable);
}
/* }}} */

/* {{{ proto void V8Js::setTimeLimit(int time_limit)
 */
static PHP_METHOD(V8Js, setTimeLimit)
{
	v8js_ctx *c;
	long time_limit = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &time_limit) == FAILURE) {
		return;
	}

	c = Z_V8JS_CTX_OBJ_P(getThis());
	c->time_limit = time_limit;

	V8JSG(timer_mutex).lock();
	for (std::deque< v8js_timer_ctx* >::iterator it = V8JSG(timer_stack).begin();
		 it != V8JSG(timer_stack).end(); it ++) {
		if((*it)->ctx == c && !(*it)->killed) {
			(*it)->time_limit = time_limit;

			// Calculate the time point when the time limit is exceeded
			std::chrono::milliseconds duration(time_limit);
			std::chrono::time_point<std::chrono::high_resolution_clock> from = std::chrono::high_resolution_clock::now();
			(*it)->time_point = from + duration;
		}
	}
	V8JSG(timer_mutex).unlock();

	if (c->in_execution && time_limit && !V8JSG(timer_thread)) {
		/* If timer thread is not started already and we now impose a time limit
		 * finally install the timer. */
		V8JSG(timer_thread) = new std::thread(v8js_timer_thread, ZEND_MODULE_GLOBALS_BULK(v8js));
	}
}
/* }}} */

/* {{{ proto void V8Js::setMemoryLimit(int memory_limit)
 */
static PHP_METHOD(V8Js, setMemoryLimit)
{
	v8js_ctx *c;
	long memory_limit = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &memory_limit) == FAILURE) {
		return;
	}

	if (memory_limit < 0) {
		zend_throw_exception(php_ce_v8js_exception,
				"memory_limit must not be negative", 0);
		return;
	}

	c = Z_V8JS_CTX_OBJ_P(getThis());
	c->memory_limit = static_cast<size_t>(memory_limit);

	V8JSG(timer_mutex).lock();
	for (std::deque< v8js_timer_ctx* >::iterator it = V8JSG(timer_stack).begin();
		 it != V8JSG(timer_stack).end(); it ++) {
		if((*it)->ctx == c && !(*it)->killed) {
			(*it)->memory_limit = static_cast<size_t>(memory_limit);
		}
	}
	V8JSG(timer_mutex).unlock();

	if (c->in_execution && memory_limit && !V8JSG(timer_thread)) {
		/* If timer thread is not started already and we now impose a memory limit
		 * finally install the timer. */
		V8JSG(timer_thread) = new std::thread(v8js_timer_thread, ZEND_MODULE_GLOBALS_BULK(v8js));
	}
}
/* }}} */

/* {{{ proto void V8Js::setAverageObjectSize(average_object_size)
 */
static PHP_METHOD(V8Js, setAverageObjectSize)
{
	v8js_ctx *c;
	long average_object_size = 0;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &average_object_size) == FAILURE) {
		return;
	}

	c = Z_V8JS_CTX_OBJ_P(getThis());
	c->average_object_size = average_object_size;
}
/* }}} */

static void v8js_persistent_zval_ctor(zval *p) /* {{{ */
{
	assert(Z_TYPE_P(p) == IS_STRING);
	Z_STR_P(p) = zend_string_dup(Z_STR_P(p), 1);
}
/* }}} */

static void v8js_persistent_zval_dtor(zval *p) /* {{{ */
{
	assert(Z_TYPE_P(p) == IS_STRING);

	if (!ZSTR_IS_INTERNED(Z_STR_P(p))) {
		free(Z_STR_P(p));
	}
}
/* }}} */

static void v8js_script_free(v8js_script *res)
{
	efree(res->name);
	delete res->script; // does Reset()
}

static void v8js_script_dtor(zend_resource *rsrc) /* {{{ */
{
	v8js_script *res = (v8js_script *)rsrc->ptr;
	if (res) {
		if(res->ctx) {
			std::vector<v8js_script *>::iterator it = std::find(res->ctx->script_objects.begin(), res->ctx->script_objects.end(), res);
			res->ctx->script_objects.erase(it);
		}

		v8js_script_free(res);
		efree(res);
	}
}
/* }}} */



/* ## Static methods ## */

static v8::StartupData createSnapshotDataBlob(v8::SnapshotCreator *snapshot_creator, zend_string *str) /* {{{ */
{
	v8::Isolate *isolate = snapshot_creator->GetIsolate();

	{
		v8::HandleScope scope(isolate);
		v8::Local<v8::Context> context = v8::Context::New(isolate);

		v8::Context::Scope context_scope(context);
		v8::TryCatch try_catch(isolate);

		v8::Local<v8::String> source = V8JS_ZSTR(str);
		v8::MaybeLocal<v8::Script> script = v8::Script::Compile(context, source);

		if (script.IsEmpty() || script.ToLocalChecked()->Run(context).IsEmpty())
		{
			return {nullptr, 0};
		}

		snapshot_creator->SetDefaultContext(context);
	}

	return snapshot_creator->CreateBlob(v8::SnapshotCreator::FunctionCodeHandling::kClear);
} /* }}} */


/* {{{ proto string|bool V8Js::createSnapshot(string embed_source)
 */
static PHP_METHOD(V8Js, createSnapshot)
{
	zend_string *script;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &script) == FAILURE) {
		return;
	}

	if (!ZSTR_LEN(script)) {
		php_error_docref(NULL, E_WARNING, "Script cannot be empty");
		RETURN_FALSE;
	}

	/* Initialize V8, if not already done. */
	v8js_v8_init();

	v8::Isolate *isolate = v8::Isolate::Allocate();
	v8::SnapshotCreator snapshot_creator(isolate);
	v8::StartupData snapshot_blob = createSnapshotDataBlob(&snapshot_creator, script);

	if (!snapshot_blob.data) {
		php_error_docref(NULL, E_WARNING, "Failed to create V8 heap snapshot.  Check $embed_source for errors.");
		RETURN_FALSE;
	}

	RETVAL_STRINGL(snapshot_blob.data, snapshot_blob.raw_size);
	delete[] snapshot_blob.data;
}
/* }}} */


/* {{{ arginfo */
ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_construct, 0, 0, 0)
	ZEND_ARG_INFO(0, object_name)
	ZEND_ARG_INFO(0, variables)
	ZEND_ARG_INFO(0, snapshot_blob)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_v8js_sleep, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_v8js_wakeup, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_executestring, 0, 0, 1)
	ZEND_ARG_INFO(0, script)
	ZEND_ARG_INFO(0, identifier)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, time_limit)
	ZEND_ARG_INFO(0, memory_limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_compilestring, 0, 0, 1)
	ZEND_ARG_INFO(0, script)
	ZEND_ARG_INFO(0, identifier)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_executescript, 0, 0, 1)
	ZEND_ARG_INFO(0, script)
	ZEND_ARG_INFO(0, flags)
	ZEND_ARG_INFO(0, time_limit)
	ZEND_ARG_INFO(0, memory_limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_checkstring, 0, 0, 1)
	ZEND_ARG_INFO(0, script)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_setmodulenormaliser, 0, 0, 2)
	ZEND_ARG_INFO(0, base)
	ZEND_ARG_INFO(0, module_id)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_setmoduleloader, 0, 0, 1)
	ZEND_ARG_INFO(0, callable)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_setaverageobjectsize, 0, 0, 1)
	ZEND_ARG_INFO(0, average_object_size)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_createsnapshot, 0, 0, 1)
	ZEND_ARG_INFO(0, script)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_settimelimit, 0, 0, 1)
	ZEND_ARG_INFO(0, time_limit)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_v8js_setmemorylimit, 0, 0, 1)
	ZEND_ARG_INFO(0, memory_limit)
ZEND_END_ARG_INFO()


const zend_function_entry v8js_methods[] = { /* {{{ */
	PHP_ME(V8Js,	__construct,			arginfo_v8js_construct,				ZEND_ACC_PUBLIC|ZEND_ACC_CTOR)
	PHP_ME(V8Js,	__sleep,				arginfo_v8js_sleep,					ZEND_ACC_PUBLIC|ZEND_ACC_FINAL)
	PHP_ME(V8Js,	__wakeup,				arginfo_v8js_wakeup,				ZEND_ACC_PUBLIC|ZEND_ACC_FINAL)
	PHP_ME(V8Js,	executeString,			arginfo_v8js_executestring,			ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	compileString,			arginfo_v8js_compilestring,			ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,    executeScript,			arginfo_v8js_executescript,			ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	setModuleNormaliser,	arginfo_v8js_setmodulenormaliser,	ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	setModuleLoader,		arginfo_v8js_setmoduleloader,		ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	setTimeLimit,			arginfo_v8js_settimelimit,			ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	setMemoryLimit,			arginfo_v8js_setmemorylimit,		ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	setAverageObjectSize,	arginfo_v8js_setaverageobjectsize,	ZEND_ACC_PUBLIC)
	PHP_ME(V8Js,	createSnapshot,			arginfo_v8js_createsnapshot,		ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	{NULL, NULL, NULL}
};
/* }}} */



/* V8Js object handlers */
static zval* v8js_write_property(zend_object *object, zend_string *member, zval *value, void **cache_slot) /* {{{ */
{
	v8js_ctx *c = Z_V8JS_CTX_OBJ(object);
	V8JS_CTX_PROLOGUE_EX(c, value);

	/* Check whether member is public, if so, export to V8. */
	zend_property_info *property_info = zend_get_property_info(c->std.ce, member, 1);

	if(!property_info ||
	   (property_info != ZEND_WRONG_PROPERTY_INFO &&
		(property_info->flags & ZEND_ACC_PUBLIC))) {
		/* Global PHP JS object */
		v8::Local<v8::String> object_name_js = v8::Local<v8::String>::New(isolate, c->object_name);
		v8::Local<v8::Object> jsobj = V8JS_GLOBAL(isolate)->Get(v8_context, object_name_js).ToLocalChecked()->ToObject(v8_context).ToLocalChecked();

		if (ZSTR_LEN(member) > std::numeric_limits<int>::max()) {
				zend_throw_exception(php_ce_v8js_exception,
						"Property name exceeds maximum supported length", 0);
				return value;
		}

		/* Write value to PHP JS object */
		v8::Local<v8::Name> key = V8JS_SYML(ZSTR_VAL(member), static_cast<int>(ZSTR_LEN(member)));
		jsobj->DefineOwnProperty(v8_context, key, zval_to_v8js(value, isolate), v8::ReadOnly);
	}

	/* Write value to PHP object */
	return std_object_handlers.write_property(object, member, value, NULL);
}
/* }}} */

static void v8js_unset_property(zend_object *object, zend_string *member, void **cache_slot) /* {{{ */
{
	V8JS_BEGIN_CTX_OBJ(c, object);
	/* Global PHP JS object */
	v8::Local<v8::String> object_name_js = v8::Local<v8::String>::New(isolate, c->object_name);
	v8::Local<v8::Object> jsobj = V8JS_GLOBAL(isolate)->Get(v8_context, object_name_js).ToLocalChecked()->ToObject(v8_context).ToLocalChecked();

	if (ZSTR_LEN(member) > std::numeric_limits<int>::max()) {
					zend_throw_exception(php_ce_v8js_exception,
					"Property name exceeds maximum supported length", 0);
			return;
	}

	/* Delete value from PHP JS object */
	v8::Local<v8::Value> key = V8JS_SYML(ZSTR_VAL(member), static_cast<int>(ZSTR_LEN(member)));

	jsobj->Delete(v8_context, key);

	/* Unset from PHP object */
	std_object_handlers.unset_property(object, member, NULL);
}
/* }}} */

PHP_MINIT_FUNCTION(v8js_class) /* {{{ */
{
	zend_class_entry ce;

	/* V8Js Class */
	INIT_CLASS_ENTRY(ce, "V8Js", v8js_methods);
	php_ce_v8js = zend_register_internal_class(&ce);
	php_ce_v8js->create_object = v8js_new;

	/* V8Js handlers */
	memcpy(&v8js_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	v8js_object_handlers.clone_obj = NULL;
	v8js_object_handlers.write_property = v8js_write_property;
	v8js_object_handlers.unset_property = v8js_unset_property;

	/* V8Js Class Constants */
	zend_declare_class_constant_string(php_ce_v8js, ZEND_STRL("V8_VERSION"),		PHP_V8_VERSION);

	zend_declare_class_constant_long(php_ce_v8js, ZEND_STRL("FLAG_NONE"),			V8JS_FLAG_NONE);
	zend_declare_class_constant_long(php_ce_v8js, ZEND_STRL("FLAG_FORCE_ARRAY"),	V8JS_FLAG_FORCE_ARRAY);
	zend_declare_class_constant_long(php_ce_v8js, ZEND_STRL("FLAG_PROPAGATE_PHP_EXCEPTIONS"), V8JS_FLAG_PROPAGATE_PHP_EXCEPTIONS);

	le_v8js_script = zend_register_list_destructors_ex(v8js_script_dtor, NULL, PHP_V8JS_SCRIPT_RES_NAME, module_number);

	return SUCCESS;
} /* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
