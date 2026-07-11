#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_PATCH_RESOURCES_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_PATCH_RESOURCES_H_

#include <string_view>

namespace content {
class WebUIDataSource;
}  // namespace content

namespace custom_browser::patch_webui {

// Returns the file name (final path component) under which a patch resource at
// |path| is registered on its WebUI data source. Patch resources are registered
// by file name -- not by their source-tree subdirectory -- so that page-relative
// imports such as `import('./about_patch.js')` resolve regardless of which
// subdirectory the module lives in (e.g. the resource generated from
// `settings/about/about_patch.js` is served as `chrome://settings/about_patch.js`,
// matching the loader's flat import). Filenames must therefore be unique per
// host. Header-only + dependency-free so it can be unit tested in isolation.
inline std::string_view PatchResourceRegistrationName(std::string_view path) {
  const std::string_view::size_type last_slash = path.find_last_of('/');
  if (last_slash == std::string_view::npos) {
    return path;
  }
  return path.substr(last_slash + 1);
}

// Adds any grit-packed resources whose path contains |host| to |data_source|.
void AddPatchResourcesForHost(std::string_view host,
                              content::WebUIDataSource* data_source);

// Adds common strings shared across multiple WebUI hosts (mode strings, pref paths).
void AddCommonPatchStrings(content::WebUIDataSource* data_source);

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_PATCH_RESOURCES_H_
