# these are the variables your Makefile.am should set
# the example is based on the colorbalance interface

#glib_enum_headers=$(colorbalance_headers)
#glib_enum_define=GST_COLOR_BALANCE
#glib_gen_prefix=gst_color_balance
#glib_gen_basename=colorbalance

hash:=\#
enum_headers=$(foreach h,$(glib_enum_headers),\n$(hash)include \"$(h)\")

# these are all the rules generating the relevant files
$(glib_gen_basename)-enumtypes.h: $(glib_enum_headers)
	$(AM_V_GEN)$(GLIB_MKENUMS) \
	--fhead "#ifndef __$(glib_enum_define)_ENUM_TYPES_H__\n#define __$(glib_enum_define)_ENUM_TYPES_H__\n\n$(glib_gen_decl_include)\n\nG_BEGIN_DECLS\n" \
	--fprod "\n/* enumerations from \"@filename@\" */\n" \
	--vhead "$(glib_gen_decl_banner)\nGType @enum_name@_get_type (void);\n#define FS_TYPE_@ENUMSHORT@ (@enum_name@_get_type())\n"         \
	--ftail "G_END_DECLS\n\n#endif /* __$(glib_enum_define)_ENUM_TYPES_H__ */" \
	$^ > $@

$(glib_gen_basename)-enumtypes.c: $(glib_enum_headers)
	@if test "x$(glib_enum_headers)" = "x"; then echo "ERROR: glib_enum_headers is empty, please fix Makefile"; exit 1; fi
	$(AM_V_GEN)$(GLIB_MKENUMS) \
	--fhead "#ifdef HAVE_CONFIG_H\n#include \"config.h\"\n#endif\n\n#include \"$(glib_gen_basename)-enumtypes.h\"\n$(enum_headers)" \
	--fprod "\n/* enumerations from \"@filename@\" */" \
	--vhead "GType\n@enum_name@_get_type (void)\n{\n  static gsize g_define_type_id__volatile = 0;\n  if (g_once_init_enter (&g_define_type_id__volatile)) {\n    static const G@Type@Value values[] = {"     \
	--vprod "      { @VALUENAME@, \"@VALUENAME@\", \"@valuenick@\" }," \
	--vtail "      { 0, NULL, NULL }\n    };\n    GType g_define_type_id = g_@type@_register_static (\"@EnumName@\", values);\n    g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);\n  }\n  return g_define_type_id__volatile;\n}\n" \
	$^ > $@

# a hack rule to make sure .Plo files exist because they get include'd
# from Makefile's
.deps/%-enumtypes.Plo:
	@touch $@
