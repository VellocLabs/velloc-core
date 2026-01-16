from branding.message import PatchTranslateNodeMessage
from branding.node import ReplaceIncludeNodeFile
from config.config import (set_branding_name,
                           set_custom_module_dir)


def apply_translate_node_message_patch():
    """Apply the patch to the Translate method of MessageNode."""
    from grit.node import message

    original_Translate = message.MessageNode.Translate

    def patched_Translate(self, lang, gender):
        result = original_Translate(self, lang, gender)
        return PatchTranslateNodeMessage(self, lang, gender, result)
    message.MessageNode.Translate = patched_Translate

def apply_node_get_input_path_patch():
    """Apply the patch to the GetInputFile method of IncludeNode."""
    from grit.node import include
    original_GetInputPath = include.IncludeNode.GetInputPath

    def patched_GetInputPath(self):
        file_path = original_GetInputPath(self)
        return ReplaceIncludeNodeFile(self, file_path)
    include.IncludeNode.GetInputPath = patched_GetInputPath

def apply_node_to_realpath_patch():
    """Log the resolved path in Node.ToRealPath."""
    from grit.node import base

    original_ToRealPath = base.Node.ToRealPath

    def patched_ToRealPath(self, path_from_basedir):
        real_path = original_ToRealPath(self, path_from_basedir)
        print(
            f"[Node.ToRealPath] path_from_basedir={path_from_basedir} "
            f"real_path={real_path}"
        )
        return real_path

    base.Node.ToRealPath = patched_ToRealPath

def apply_patches(custom_module_dir: str, branding_name: str):
    """Apply patches to the grit module."""
    set_custom_module_dir(custom_module_dir)
    set_branding_name(branding_name)
    apply_translate_node_message_patch()
    #apply_node_get_input_path_patch()
    #apply_node_to_realpath_patch()

  #  hook read file: tools\grit\grit\util.py
  # with open(filename, mode, encoding=encoding) as f:
  #    return f.read()
