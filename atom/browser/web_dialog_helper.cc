// Copyright (c) 2014 GitHub, Inc. All rights reserved.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/web_dialog_helper.h"

#include <string>
#include <utility>
#include <vector>

#include "atom/browser/atom_browser_context.h"
#include "atom/browser/native_window.h"
#include "atom/browser/ui/file_dialog.h"
#include "atom/common/native_mate_converters/once_callback.h"
#include "base/bind.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "native_mate/dictionary.h"
#include "net/base/mime_util.h"
#include "ui/shell_dialogs/selected_file_info.h"

using blink::mojom::FileChooserFileInfo;
using blink::mojom::FileChooserFileInfoPtr;
using blink::mojom::FileChooserParams;

namespace {

class FileSelectHelper : public base::RefCounted<FileSelectHelper>,
                         public content::WebContentsObserver,
                         public atom::DirectoryListerHelperDelegate {
 public:
  FileSelectHelper(content::RenderFrameHost* render_frame_host,
                   std::unique_ptr<content::FileSelectListener> listener,
                   blink::mojom::FileChooserParams::Mode mode)
      : render_frame_host_(render_frame_host),
        listener_(std::move(listener)),
        mode_(mode) {
    auto* web_contents =
        content::WebContents::FromRenderFrameHost(render_frame_host);
    content::WebContentsObserver::Observe(web_contents);
  }

  void ShowOpenDialog(const file_dialog::DialogSettings& settings) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    atom::util::Promise promise(isolate);

    auto callback = base::BindOnce(&FileSelectHelper::OnOpenDialogDone, this);
    ignore_result(promise.Then(std::move(callback)));

    file_dialog::ShowOpenDialog(settings, std::move(promise));
  }

  void ShowSaveDialog(const file_dialog::DialogSettings& settings) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    atom::util::Promise promise(isolate);
    v8::Local<v8::Promise> handle = promise.GetHandle();

    file_dialog::ShowSaveDialog(settings, std::move(promise));
    ignore_result(handle->Then(
        context,
        v8::Local<v8::Function>::Cast(mate::ConvertToV8(
            isolate,
            base::BindOnce(&FileSelectHelper::OnSaveDialogDone, this)))));
  }

  void OnDirectoryListerDone(std::vector<FileChooserFileInfoPtr> file_info,
                             base::FilePath base_dir) override {
    OnFilesSelected(std::move(file_info), base_dir);
    Release();
  }

 private:
  friend class base::RefCounted<FileSelectHelper>;

  ~FileSelectHelper() override {}

  void EnumerateDirectory(base::FilePath base_dir) {
    auto* lister = new net::DirectoryLister(
        base_dir, net::DirectoryLister::NO_SORT_RECURSIVE,
        new atom::DirectoryListerHelper(base_dir, this));
    lister->Start();
    // It is difficult for callers to know how long to keep a reference to
    // this instance.  We AddRef() here to keep the instance alive after we
    // return to the caller.  Once the directory lister is complete we
    // Release() in OnDirectoryListerDone() and at that point we run
    // OnFilesSelected() which will deref the last reference held by the
    // listener.
    AddRef();
  }

  void OnOpenDialogDone(mate::Dictionary result) {
    std::vector<FileChooserFileInfoPtr> file_info;
    bool canceled = true;
    result.Get("canceled", &canceled);
    base::FilePath base_dir;
    // For certain file chooser modes (kUploadFolder) we need to do some async
    // work before calling back to the listener.  In that particular case the
    // listener is called from the directory enumerator.
    bool ready_to_call_listener = false;

    if (!canceled) {
      std::vector<base::FilePath> paths;
      if (result.Get("filePaths", &paths)) {
        // If we are uploading a folder we need to enumerate its contents
        if (mode_ == FileChooserParams::Mode::kUploadFolder &&
            paths.size() >= 1) {
          base_dir = paths[0];

          // Actually enumerate soemwhere off-thread
          base::SequencedTaskRunnerHandle::Get()->PostTask(
              FROM_HERE, base::BindOnce(&FileSelectHelper::EnumerateDirectory,
                                        this, base_dir));
        } else {
          for (auto& path : paths) {
            file_info.push_back(FileChooserFileInfo::NewNativeFile(
                blink::mojom::NativeFileInfo::New(
                    path, path.BaseName().AsUTF16Unsafe())));
          }

          ready_to_call_listener = true;
        }

        if (render_frame_host_ && !paths.empty()) {
          auto* browser_context = static_cast<atom::AtomBrowserContext*>(
              render_frame_host_->GetProcess()->GetBrowserContext());
          browser_context->prefs()->SetFilePath(prefs::kSelectFileLastDirectory,
                                                paths[0].DirName());
        }
      }
    }

    if (ready_to_call_listener)
      OnFilesSelected(std::move(file_info), base_dir);
  }

  void OnSaveDialogDone(mate::Dictionary result) {
    std::vector<FileChooserFileInfoPtr> file_info;
    bool canceled = true;
    result.Get("canceled", &canceled);

    if (!canceled) {
      base::FilePath path;
      if (result.Get("filePath", &path)) {
        file_info.push_back(FileChooserFileInfo::NewNativeFile(
            blink::mojom::NativeFileInfo::New(
                path, path.BaseName().AsUTF16Unsafe())));
      }
    }
    OnFilesSelected(std::move(file_info), base::FilePath());
  }

  void OnFilesSelected(std::vector<FileChooserFileInfoPtr> file_info,
                       base::FilePath base_dir) {
    if (listener_) {
      listener_->FileSelected(std::move(file_info), base_dir, mode_);
      listener_.reset();
    }
    render_frame_host_ = nullptr;
  }

  // content::WebContentsObserver:
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override {
    if (old_host == render_frame_host_)
      render_frame_host_ = nullptr;
  }

  // content::WebContentsObserver:
  void RenderFrameDeleted(content::RenderFrameHost* deleted_host) override {
    if (deleted_host == render_frame_host_)
      render_frame_host_ = nullptr;
  }

  // content::WebContentsObserver:
  void WebContentsDestroyed() override { render_frame_host_ = nullptr; }

  content::RenderFrameHost* render_frame_host_;
  std::unique_ptr<content::FileSelectListener> listener_;
  blink::mojom::FileChooserParams::Mode mode_;
};

file_dialog::Filters GetFileTypesFromAcceptType(
    const std::vector<base::string16>& accept_types) {
  file_dialog::Filters filters;
  if (accept_types.empty())
    return filters;

  std::vector<base::FilePath::StringType> extensions;

  int valid_type_count = 0;
  std::string description;

  for (const auto& accept_type : accept_types) {
    std::string ascii_type = base::UTF16ToASCII(accept_type);
    auto old_extension_size = extensions.size();

    if (ascii_type[0] == '.') {
      // If the type starts with a period it is assumed to be a file extension,
      // like `.txt`, // so we just have to add it to the list.
      base::FilePath::StringType extension(ascii_type.begin(),
                                           ascii_type.end());
      // Skip the first character.
      extensions.push_back(extension.substr(1));
    } else {
      if (ascii_type == "image/*")
        description = "Image Files";
      else if (ascii_type == "audio/*")
        description = "Audio Files";
      else if (ascii_type == "video/*")
        description = "Video Files";

      // For MIME Type, `audio/*, video/*, image/*
      net::GetExtensionsForMimeType(ascii_type, &extensions);
    }

    if (extensions.size() > old_extension_size)
      valid_type_count++;
  }

  // If no valid exntesion is added, return empty filters.
  if (extensions.empty())
    return filters;

  filters.push_back(file_dialog::Filter());

  if (valid_type_count > 1 || (valid_type_count == 1 && description.empty()))
    description = "Custom Files";

  DCHECK(!description.empty());
  filters[0].first = description;

  for (const auto& extension : extensions) {
#if defined(OS_WIN)
    filters[0].second.push_back(base::UTF16ToASCII(extension));
#else
    filters[0].second.push_back(extension);
#endif
  }

  // Allow all files when extension is specified.
  filters.push_back(file_dialog::Filter());
  filters.back().first = "All Files";
  filters.back().second.push_back("*");

  return filters;
}

}  // namespace

namespace atom {

DirectoryListerHelper::DirectoryListerHelper(
    base::FilePath base,
    DirectoryListerHelperDelegate* helper)
    : base_dir_(base), delegate_(helper) {}
DirectoryListerHelper::~DirectoryListerHelper() {}

void DirectoryListerHelper::OnListFile(
    const net::DirectoryLister::DirectoryListerData& data) {
  // We don't want to return directory paths, only file paths
  if (data.info.IsDirectory())
    return;

  paths_.push_back(data.path);
}
void DirectoryListerHelper::OnListDone(int error) {
  std::vector<FileChooserFileInfoPtr> file_info;
  for (auto path : paths_)
    file_info.push_back(FileChooserFileInfo::NewNativeFile(
        blink::mojom::NativeFileInfo::New(path, base::string16())));

  delegate_->OnDirectoryListerDone(std::move(file_info), base_dir_);
  delete this;
}

WebDialogHelper::WebDialogHelper(NativeWindow* window, bool offscreen)
    : window_(window), offscreen_(offscreen), weak_factory_(this) {}

WebDialogHelper::~WebDialogHelper() {}

void WebDialogHelper::RunFileChooser(
    content::RenderFrameHost* render_frame_host,
    std::unique_ptr<content::FileSelectListener> listener,
    const blink::mojom::FileChooserParams& params) {
  file_dialog::DialogSettings settings;
  settings.force_detached = offscreen_;
  settings.filters = GetFileTypesFromAcceptType(params.accept_types);
  settings.parent_window = window_;
  settings.title = base::UTF16ToUTF8(params.title);

  scoped_refptr<FileSelectHelper> file_select_helper(new FileSelectHelper(
      render_frame_host, std::move(listener), params.mode));
  if (params.mode == FileChooserParams::Mode::kSave) {
    settings.default_path = params.default_file_name;
    file_select_helper->ShowSaveDialog(settings);
  } else {
    int flags = file_dialog::FILE_DIALOG_CREATE_DIRECTORY;
    switch (params.mode) {
      case FileChooserParams::Mode::kOpenMultiple:
        flags |= file_dialog::FILE_DIALOG_MULTI_SELECTIONS;
        FALLTHROUGH;
      case FileChooserParams::Mode::kOpen:
        flags |= file_dialog::FILE_DIALOG_OPEN_FILE;
        flags |= file_dialog::FILE_DIALOG_TREAT_PACKAGE_APP_AS_DIRECTORY;
        break;
      case FileChooserParams::Mode::kUploadFolder:
        flags |= file_dialog::FILE_DIALOG_OPEN_DIRECTORY;
        break;
      default:
        NOTREACHED();
    }

    auto* browser_context = static_cast<atom::AtomBrowserContext*>(
        render_frame_host->GetProcess()->GetBrowserContext());
    settings.default_path = browser_context->prefs()
                                ->GetFilePath(prefs::kSelectFileLastDirectory)
                                .Append(params.default_file_name);
    settings.properties = flags;
    file_select_helper->ShowOpenDialog(settings);
  }
}

void WebDialogHelper::EnumerateDirectory(
    content::WebContents* web_contents,
    std::unique_ptr<content::FileSelectListener> listener,
    const base::FilePath& dir) {
  int types = base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES |
              base::FileEnumerator::INCLUDE_DOT_DOT;
  base::FileEnumerator file_enum(dir, false, types);

  base::FilePath path;
  std::vector<FileChooserFileInfoPtr> file_info;
  while (!(path = file_enum.Next()).empty()) {
    file_info.push_back(FileChooserFileInfo::NewNativeFile(
        blink::mojom::NativeFileInfo::New(path, base::string16())));
  }

  listener->FileSelected(std::move(file_info), dir,
                         FileChooserParams::Mode::kUploadFolder);
}

}  // namespace atom
