// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/permissions/site_permissions.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"

namespace lrb {

namespace {

constexpr AskablePermission kAskable[] = {
    {blink::PermissionType::VIDEO_CAPTURE, "camera", u"use your camera",
     u"Camera"},
    {blink::PermissionType::AUDIO_CAPTURE, "microphone",
     u"use your microphone", u"Microphone"},
    {blink::PermissionType::CLIPBOARD_READ_WRITE, "clipboard-read",
     u"see what you copy", u"Clipboard reading"},
};

}  // namespace

const AskablePermission* FindAskable(blink::PermissionType type) {
  for (const AskablePermission& askable : kAskable) {
    if (askable.type == type) {
      return &askable;
    }
  }
  return nullptr;
}

SitePermissions::SitePermissions(const base::FilePath& file)
    : file_(file),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  if (file_.empty()) {
    return;
  }
  file_task_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath file) -> std::optional<std::string> {
            std::string json;
            if (!base::ReadFileToString(file, &json)) {
              return std::nullopt;
            }
            return json;
          },
          file_),
      base::BindOnce(&SitePermissions::OnLoaded, weak_factory_.GetWeakPtr()));
}

SitePermissions::~SitePermissions() = default;

void SitePermissions::OnLoaded(std::optional<std::string> json) {
  if (!json) {
    return;
  }
  std::optional<base::DictValue> loaded =
      base::JSONReader::ReadDict(*json, base::JSON_PARSE_RFC);
  if (!loaded) {
    LOG(WARNING) << "lrb: unreadable " << file_;
    return;
  }
  // Answers given before the file was read win.
  loaded->Merge(std::move(answers_));
  answers_ = std::move(*loaded);
}

std::optional<bool> SitePermissions::Get(const url::Origin& origin,
                                         blink::PermissionType type) const {
  const AskablePermission* askable = FindAskable(type);
  const base::DictValue* site = answers_.FindDict(origin.Serialize());
  if (!askable || !site) {
    return std::nullopt;
  }
  return site->FindBool(askable->key);
}

void SitePermissions::Set(const url::Origin& origin,
                          blink::PermissionType type,
                          bool allowed) {
  const AskablePermission* askable = FindAskable(type);
  if (!askable || origin.opaque()) {
    return;
  }
  base::DictValue* site = answers_.EnsureDict(origin.Serialize());
  site->Set(askable->key, allowed);
  Save();
}

void SitePermissions::Forget(const url::Origin& origin,
                             blink::PermissionType type) {
  const AskablePermission* askable = FindAskable(type);
  base::DictValue* site = answers_.FindDict(origin.Serialize());
  if (!askable || !site || !site->Remove(askable->key)) {
    return;
  }
  if (site->empty()) {
    answers_.Remove(origin.Serialize());
  }
  Save();
}

std::vector<std::pair<blink::PermissionType, bool>> SitePermissions::List(
    const url::Origin& origin) const {
  std::vector<std::pair<blink::PermissionType, bool>> list;
  for (const AskablePermission& askable : kAskable) {
    if (std::optional<bool> answer = Get(origin, askable.type)) {
      list.emplace_back(askable.type, *answer);
    }
  }
  return list;
}

void SitePermissions::Save() {
  if (file_.empty()) {
    return;
  }
  std::optional<std::string> json = base::WriteJson(answers_);
  file_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::FilePath file, std::string json) {
            if (!base::ImportantFileWriter::WriteFileAtomically(file, json)) {
              LOG(ERROR) << "lrb: could not save " << file;
            }
          },
          file_, json.value_or("{}")));
}

}  // namespace lrb
