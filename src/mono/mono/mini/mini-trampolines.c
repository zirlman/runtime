
#include <config.h>
#include <glib.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include "mini.h"

#ifdef MONO_ARCH_HAVE_IMT

static gpointer*
mono_convert_imt_slot_to_vtable_slot (gpointer* slot, gpointer *regs, MonoMethod *method) {
	MonoObject *this_argument = mono_arch_find_this_argument (regs, method);
	MonoVTable *vt = this_argument->vtable;
	int displacement = slot - ((gpointer*)vt);
	
	if (displacement > 0) {
		/* slot is in the vtable, not in the IMT */
#if DEBUG_IMT
		printf ("mono_convert_imt_slot_to_vtable_slot: slot %p is in the vtable, not in the IMT\n", slot);
#endif
		return slot;
	} else {
		MonoMethod *imt_method = mono_arch_find_imt_method (regs);
		int interface_offset = mono_class_interface_offset (vt->klass, imt_method->klass);
		int imt_slot = MONO_IMT_SIZE + displacement;

#if DEBUG_IMT
		printf ("mono_convert_imt_slot_to_vtable_slot: method = %s.%s.%s, imt_method = %s.%s.%s\n",
				method->klass->name_space, method->klass->name, method->name, 
				imt_method->klass->name_space, imt_method->klass->name, imt_method->name);
#endif
		g_assert (imt_slot < MONO_IMT_SIZE);
		if (vt->imt_collisions_bitmap & (1 << imt_slot)) {
			int vtable_offset = interface_offset + imt_method->slot;
			gpointer *vtable_slot = & (vt->vtable [vtable_offset]);
#if DEBUG_IMT
			printf ("mono_convert_imt_slot_to_vtable_slot: slot %p[%d] is in the IMT, and colliding becomes %p[%d] (interface_offset = %d, method->slot = %d)\n", slot, imt_slot, vtable_slot, vtable_offset, interface_offset, imt_method->slot);
#endif
			g_assert (vtable_offset >= 0);
			return vtable_slot;
		} else {
#if DEBUG_IMT
			printf ("mono_convert_imt_slot_to_vtable_slot: slot %p[%d] is in the IMT, but not colliding\n", slot, imt_slot);
#endif
			return slot;
		}
	}
}
#endif

/**
 * mono_magic_trampoline:
 *
 *   This trampoline handles calls from JITted code.
 */
gpointer
mono_magic_trampoline (gssize *regs, guint8 *code, MonoMethod *m, guint8* tramp)
{
	gpointer addr;
	gpointer *vtable_slot;

	addr = mono_compile_method (m);
	g_assert (addr);

	/* the method was jumped to */
	if (!code)
		return addr;

	vtable_slot = mono_arch_get_vcall_slot_addr (code, (gpointer*)regs);

	if (vtable_slot) {
		if (m->klass->valuetype)
			addr = mono_arch_get_unbox_trampoline (m, addr);

		g_assert (*vtable_slot);

		if (mono_aot_is_got_entry (code, (guint8*)vtable_slot) || mono_domain_owns_vtable_slot (mono_domain_get (), vtable_slot)) {
#ifdef MONO_ARCH_HAVE_IMT
			vtable_slot = mono_convert_imt_slot_to_vtable_slot (vtable_slot, (gpointer*)regs, m);
#endif
			*vtable_slot = mono_get_addr_from_ftnptr (addr);
		}
	}
	else {
		guint8 *plt_entry = mono_aot_get_plt_entry (code);

		/* Patch calling code */
		if (plt_entry) {
			mono_arch_patch_plt_entry (plt_entry, addr);
		} else {
			MonoJitInfo *ji = 
				mono_jit_info_table_find (mono_domain_get (), (char*)code);
			MonoJitInfo *target_ji = 
				mono_jit_info_table_find (mono_domain_get (), mono_get_addr_from_ftnptr (addr));

			if (mono_method_same_domain (ji, target_ji))
				mono_arch_patch_callsite (code, addr);
		}
	}

	return addr;
}

/*
 * mono_aot_trampoline:
 *
 *   This trampoline handles calls made from AOT code. We try to bypass the 
 * normal JIT compilation logic to avoid loading the metadata for the method.
 */
#ifdef MONO_ARCH_HAVE_CREATE_TRAMPOLINE_FROM_TOKEN
gpointer
mono_aot_trampoline (gssize *regs, guint8 *code, guint8 *token_info, 
					 guint8* tramp)
{
	MonoImage *image;
	guint32 token;
	MonoMethod *method = NULL;
	gpointer addr;
	gpointer *vtable_slot;
	gboolean is_got_entry;

	image = *(gpointer*)(gpointer)token_info;
	token_info += sizeof (gpointer);
	token = *(guint32*)(gpointer)token_info;

	addr = mono_aot_get_method_from_token (mono_domain_get (), image, token);
	if (!addr) {
		method = mono_get_method (image, token, NULL);
		g_assert (method);

		//printf ("F: %s\n", mono_method_full_name (method, TRUE));

		if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
			method = mono_marshal_get_synchronized_wrapper (method);

		addr = mono_compile_method (method);
		g_assert (addr);
	}

	vtable_slot = mono_arch_get_vcall_slot_addr (code, (gpointer*)regs);

	if (vtable_slot) {
		is_got_entry = mono_aot_is_got_entry (code, (guint8*)vtable_slot);

		if (!is_got_entry) {
			if (!method)
				method = mono_get_method (image, token, NULL);
			if (method->klass->valuetype)
				addr = mono_arch_get_unbox_trampoline (method, addr);
		}
	} else {
		/* This is a normal call through a PLT entry */
		guint8 *plt_entry = mono_aot_get_plt_entry (code);

		g_assert (plt_entry);

		mono_arch_patch_plt_entry (plt_entry, addr);

		is_got_entry = FALSE;
	}

	/*
	 * Since AOT code is only used in the root domain, 
	 * mono_domain_get () != mono_get_root_domain () means the calling method
	 * is AppDomain:InvokeInDomain, so this is the same check as in 
	 * mono_method_same_domain () but without loading the metadata for the method.
	 */
	if ((is_got_entry && (mono_domain_get () == mono_get_root_domain ())) || mono_domain_owns_vtable_slot (mono_domain_get (), vtable_slot))
		*vtable_slot = addr;

	return addr;
}

/*
 * mono_aot_plt_trampoline:
 *
 *   This trampoline handles calls made from AOT code through the PLT table.
 */
gpointer
mono_aot_plt_trampoline (gssize *regs, guint8 *code, guint8 *aot_module, 
						 guint8* tramp)
{
#ifdef MONO_ARCH_AOT_PLT_OFFSET_REG
	guint32 plt_info_offset = regs [MONO_ARCH_AOT_PLT_OFFSET_REG];
#else
	guint32 plt_info_offset = -1;
#endif

	return mono_aot_plt_resolve (aot_module, plt_info_offset, code);
}
#endif

/**
 * mono_class_init_trampoline:
 *
 * This method calls mono_runtime_class_init () to run the static constructor
 * for the type, then patches the caller code so it is not called again.
 */
void
mono_class_init_trampoline (gssize *regs, guint8 *code, MonoVTable *vtable, guint8 *tramp)
{
	guint8 *plt_entry = mono_aot_get_plt_entry (code);

	mono_runtime_class_init (vtable);

	if (!mono_running_on_valgrind ()) {
		if (plt_entry) {
			mono_arch_nullify_plt_entry (plt_entry);
		} else {
			mono_arch_nullify_class_init_trampoline (code, regs);
		}
	}
}

#ifdef MONO_ARCH_HAVE_CREATE_DELEGATE_TRAMPOLINE

/**
 * mono_delegate_trampoline:
 *
 *   This trampoline handles calls made to Delegate:Invoke ().
 */
gpointer
mono_delegate_trampoline (gssize *regs, guint8 *code, MonoClass *klass, guint8* tramp)
{
	MonoDomain *domain = mono_domain_get ();
	MonoDelegate *delegate;
	MonoJitInfo *ji;
	gpointer iter;
	MonoMethod *invoke;
	gboolean multicast;

	/* Find the Invoke method */
	iter = NULL;
	while ((invoke = mono_class_get_methods (klass, &iter))) {
		if (!strcmp (invoke->name, "Invoke"))
			break;
	}
	g_assert (invoke);

	/* Obtain the delegate object according to the calling convention */

	delegate = mono_arch_get_this_arg_from_call (mono_method_signature (invoke), regs, code);

	/* 
	 * If the called address is a trampoline, replace it with the compiled method so
	 * further calls don't have to go through the trampoline.
	 */
	ji = mono_jit_info_table_find (domain, mono_get_addr_from_ftnptr (delegate->method_ptr));
	if (ji)
		delegate->method_ptr = mono_compile_method (ji->method);

	multicast = ((MonoMulticastDelegate*)delegate)->prev != NULL;
	if (!multicast) {
		guint8* code;
		GHashTable *cache;

		mono_domain_lock (domain);
		if (delegate->target != NULL) {
			if (!domain->delegate_invoke_impl_with_target_hash)
				domain->delegate_invoke_impl_with_target_hash = g_hash_table_new (mono_aligned_addr_hash, NULL);
			cache = domain->delegate_invoke_impl_with_target_hash;
		} else {
			if (!domain->delegate_invoke_impl_no_target_hash)
				domain->delegate_invoke_impl_no_target_hash = g_hash_table_new (mono_aligned_addr_hash, NULL);
			cache = domain->delegate_invoke_impl_no_target_hash;
		}
		code = g_hash_table_lookup (cache, mono_method_signature (invoke));
		mono_domain_unlock (domain);
		if (code) {
			delegate->invoke_impl = code;
			return code;
		}

		code = mono_arch_get_delegate_invoke_impl (mono_method_signature (invoke), delegate->target != NULL);

		if (code) {
			mono_domain_lock (domain);
			g_hash_table_insert (cache, mono_method_signature (invoke), code);
			mono_domain_unlock (domain);

			delegate->invoke_impl = code;
			return code;
		}
	}

	/* The general, unoptimized case */
	delegate->invoke_impl = mono_compile_method (mono_marshal_get_delegate_invoke (invoke));
	return delegate->invoke_impl;
}

#endif


