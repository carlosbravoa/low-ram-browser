// Copyright 2026 The low-ram-browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lrb/browser/keyring.h"

#include <string_view>
#include <utility>

#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "lrb/browser/lrb_content_browser_client.h"
#include "lrb/browser/site.h"
#include "lrb/common/mode.h"
#include "url/gurl.h"

namespace lrb {

namespace {

// As the coordinator's (keyring_broker.cc): every line stays under 9000
// bytes, values percent-encoded.
constexpr size_t kMaxUser = 256;
constexpr size_t kMaxPassword = 1024;
constexpr size_t kMaxRecord = 2400;

// Requests waiting for their answer, by id.
struct Pending {
  base::OnceCallback<void(std::optional<std::vector<std::u16string>>)> users;
  base::OnceCallback<void(std::optional<std::u16string>)> password;
  base::OnceCallback<void(bool)> saved;
  base::OnceCallback<void(Details)> details;
};

std::map<uint64_t, Pending>& Requests() {
  static base::NoDestructor<std::map<uint64_t, Pending>> requests;
  return *requests;
}

LrbContentBrowserClient* Coordinated() {
  LrbContentBrowserClient* client = LrbContentBrowserClient::Get();
  return client && client->has_coordinator() ? client : nullptr;
}

uint64_t NextId() {
  static uint64_t next = 0;
  return ++next;
}

std::string Encode(std::string_view text) {
  return base::EscapeAllExceptUnreserved(text);
}

std::u16string Decode(std::string_view text) {
  return base::UTF8ToUTF16(base::UnescapeBinaryURLComponent(text));
}

// Autocomplete tokens: lowercase letters, digits and '-'.
bool ValidKey(const std::string& key) {
  if (key.empty() || key.size() > 40) {
    return false;
  }
  for (char c : key) {
    if (!(base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '-')) {
      return false;
    }
  }
  return true;
}

// Details as one string: "email=a%40b.cl&tel=..." (what the keyring keeps).
std::string Record(const Details& details) {
  std::string record;
  for (const auto& [key, value] : details) {
    if (!ValidKey(key) || value.empty()) {
      continue;
    }
    if (!record.empty()) {
      record += '&';
    }
    record += key + "=" + Encode(base::UTF16ToUTF8(value));
  }
  return record;
}

Details ParseRecord(std::string_view record) {
  Details details;
  for (std::string_view pair : base::SplitStringPiece(
           record, "&", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const size_t eq = pair.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string key(pair.substr(0, eq));
    if (ValidKey(key)) {
      details[key] = Decode(pair.substr(eq + 1));
    }
  }
  return details;
}

// Standard mode: the instance's profile is every site's, so the broker
// can't tell the site from it; the request names it (" <site>"). The broker
// checks that `origin` belongs to it. Lean mode: nothing, the profile says.
std::string SiteSuffix(const std::string& origin) {
  const std::string site =
      StandardMode() ? SiteForUrl(GURL(origin)) : std::string();
  // Without one (not http(s)), the standard profile names no site either:
  // the broker refuses.
  return site.empty() ? std::string() : " " + Encode(site);
}

uint64_t Add(Pending pending) {
  const uint64_t id = NextId();
  Requests()[id] = std::move(pending);
  return id;
}

}  // namespace

bool PasswordFits(const std::u16string& username,
                  const std::u16string& password) {
  const size_t user = base::UTF16ToUTF8(username).size();
  const size_t secret = base::UTF16ToUTF8(password).size();
  return user > 0 && user <= kMaxUser && secret > 0 && secret <= kMaxPassword;
}

bool DetailsFit(const Details& details) {
  const std::string record = Record(details);
  return !record.empty() && record.size() <= kMaxRecord;
}

void ListPasswords(
    const std::string& origin,
    base::OnceCallback<void(std::optional<std::vector<std::u16string>>)>
        done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client) {
    std::move(done).Run(std::nullopt);
    return;
  }
  Pending pending;
  pending.users = std::move(done);
  client->SendToCoordinator("pw-list " +
                            base::NumberToString(Add(std::move(pending))) +
                            " " + Encode(origin) + SiteSuffix(origin));
}

void GetPassword(const std::string& origin,
                 const std::u16string& username,
                 base::OnceCallback<void(std::optional<std::u16string>)> done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client || base::UTF16ToUTF8(username).size() > kMaxUser) {
    std::move(done).Run(std::nullopt);
    return;
  }
  Pending pending;
  pending.password = std::move(done);
  client->SendToCoordinator(
      "pw-get " + base::NumberToString(Add(std::move(pending))) + " " +
      Encode(origin) + " " + Encode(base::UTF16ToUTF8(username)) +
      SiteSuffix(origin));
}

void SavePassword(const std::string& origin,
                  const std::u16string& username,
                  const std::u16string& password,
                  base::OnceCallback<void(bool)> done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client || !PasswordFits(username, password)) {
    std::move(done).Run(false);
    return;
  }
  Pending pending;
  pending.saved = std::move(done);
  client->SendToCoordinator(
      "pw-save " + base::NumberToString(Add(std::move(pending))) + " " +
      Encode(origin) + " " + Encode(base::UTF16ToUTF8(username)) + " " +
      Encode(base::UTF16ToUTF8(password)) + SiteSuffix(origin));
}

void GetDetails(base::OnceCallback<void(Details)> done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client) {
    std::move(done).Run({});
    return;
  }
  Pending pending;
  pending.details = std::move(done);
  client->SendToCoordinator("af-get " +
                            base::NumberToString(Add(std::move(pending))));
}

void SaveDetails(const Details& details, base::OnceCallback<void(bool)> done) {
  LrbContentBrowserClient* client = Coordinated();
  if (!client || !DetailsFit(details)) {
    std::move(done).Run(false);
    return;
  }
  Pending pending;
  pending.saved = std::move(done);
  client->SendToCoordinator(
      "af-save " + base::NumberToString(Add(std::move(pending))) + " " +
      Encode(Record(details)));
}

bool OnKeyringLine(const std::string& line) {
  // "<answer> <id> <rest>"
  const std::vector<std::string_view> parts = base::SplitStringPiece(
      line, " ", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.size() < 3) {
    return false;
  }
  const std::string_view answer = parts[0];
  if (answer != "pw-users" && answer != "pw" && answer != "pw-saved" &&
      answer != "af" && answer != "af-saved") {
    return false;
  }
  uint64_t id = 0;
  auto it = base::StringToUint64(parts[1], &id) ? Requests().find(id)
                                                 : Requests().end();
  if (it == Requests().end()) {
    return true;  // ours, for a request that's gone
  }
  Pending pending = std::move(it->second);
  Requests().erase(it);
  const bool ok = parts[2] == "ok";
  if (answer == "pw-users" && pending.users) {
    if (!ok) {
      std::move(pending.users).Run(std::nullopt);  // no keyring
      return true;
    }
    std::vector<std::u16string> users;
    for (size_t i = 3; i < parts.size(); ++i) {
      if (!parts[i].empty()) {
        users.push_back(Decode(parts[i]));
      }
    }
    std::move(pending.users).Run(std::move(users));
  } else if (answer == "pw" && pending.password) {
    std::move(pending.password)
        .Run(ok && parts.size() > 3 ? std::optional(Decode(parts[3]))
                                    : std::nullopt);
  } else if ((answer == "pw-saved" || answer == "af-saved") && pending.saved) {
    std::move(pending.saved).Run(parts[2] == "1");
  } else if (answer == "af" && pending.details) {
    std::move(pending.details)
        .Run(ok && parts.size() > 3
                 ? ParseRecord(base::UnescapeBinaryURLComponent(parts[3]))
                 : Details());
  }
  return true;
}

}  // namespace lrb
