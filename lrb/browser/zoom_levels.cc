// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/zoom_levels.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/values.h"

namespace lrb {

ZoomLevels::ZoomLevels(content::BrowserContext* context,
                       const base::FilePath& file)
    : map_(content::HostZoomMap::GetDefaultForBrowserContext(context)),
      file_(file),
      file_task_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {
  subscription_ = map_->AddZoomLevelChangedCallback(base::BindRepeating(
      &ZoomLevels::OnChanged, weak_factory_.GetWeakPtr()));
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
      base::BindOnce(&ZoomLevels::OnLoaded, weak_factory_.GetWeakPtr()));
}

ZoomLevels::~ZoomLevels() = default;

void ZoomLevels::OnLoaded(std::optional<std::string> json) {
  if (!json) {
    return;
  }
  std::optional<base::DictValue> levels =
      base::JSONReader::ReadDict(*json, base::JSON_PARSE_RFC);
  if (!levels) {
    LOG(WARNING) << "lrb: unreadable " << file_;
    return;
  }
  loading_ = true;  // these aren't changes to save
  for (const auto [host, level] : *levels) {
    if (std::optional<double> value = level.GetIfDouble()) {
      map_->SetZoomLevelForHost(host, *value);
    }
  }
  loading_ = false;
}

void ZoomLevels::OnChanged(
    const content::HostZoomMap::ZoomLevelChange& change) {
  if (loading_ ||
      change.mode != content::HostZoomMap::ZOOM_CHANGED_FOR_HOST) {
    return;
  }
  base::DictValue levels;
  for (const content::HostZoomMap::ZoomLevelChange& level :
       map_->GetAllZoomLevels()) {
    if (level.mode == content::HostZoomMap::ZOOM_CHANGED_FOR_HOST &&
        level.zoom_level != 0) {
      levels.Set(level.host, level.zoom_level);
    }
  }
  std::optional<std::string> json = base::WriteJson(levels);
  file_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::FilePath file, std::string json) {
                       if (!base::ImportantFileWriter::WriteFileAtomically(
                               file, json)) {
                         LOG(ERROR) << "lrb: could not save " << file;
                       }
                     },
                     file_, json.value_or("{}")));
}

}  // namespace lrb
