#include <exception>
#include <string>
#include <vector>

#include <fmt/compile.h>
#include <fmt/format.h>
#include <klib/exception.h>
#include <klib/log.h>
#include <klib/unicode.h>
#include <klib/util.h>
#include <CLI/CLI.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/core/ignore_unused.hpp>

#include "http.h"
#include "json.h"
#include "progress_bar.h"
#include "util.h"
#include "version.h"

#ifndef NDEBUG
#include <backward.hpp>
backward::SignalHandling sh;
#endif

using namespace kepub::sfacg;

namespace {

bool show_user_info() {
  auto response = http_get("https://api.sfacg.com/user");
  auto info = json_to_user_info(response.text());

  if (info.login_expired_) {
    return false;
  } else {
    klib::info("Use existing cookies, nick name: {}", info.nick_name_);
    return true;
  }
}

void login(const std::string &login_name, const std::string &password) {
  auto response = http_post("https://api.sfacg.com/sessions",
                            serialize(login_name, password));
  json_base(response.text());

  response = http_get("https://api.sfacg.com/user");
  auto info = json_to_login_info(response.text());
  klib::info("Login successful, nick name: {}", info.user_info_.nick_name_);
}

kepub::BookInfo get_book_info(const std::string &book_id) {
  auto response = http_get("https://api.sfacg.com/novels/" + book_id,
                           {{"expand", "intro"}});
  auto info = json_to_book_info(response.text());

  klib::info("Book name: {}", info.name_);
  klib::info("Author: {}", info.author_);
  klib::info("Point: {}", info.point_);
  klib::info("Cover url: {}", info.cover_path_);

  std::string cover_name = "cover.jpg";
  response = http_get_rss(info.cover_path_);
  response.save_to_file(cover_name);
  klib::info("Cover downloaded successfully: {}", cover_name);

  return info;
}

std::vector<kepub::Volume> get_volume_chapter(const std::string &book_id) {
  auto response = http_get(fmt::format(
      FMT_COMPILE("https://api.sfacg.com/novels/{}/dirs"), book_id));

  return json_to_volumes(response.text());
}

std::vector<std::string> get_content(const std::string &chapter_id) {
  auto response = http_get("https://api.sfacg.com/Chaps/" + chapter_id,
                           {{"chapsId", chapter_id}, {"expand", "content"}});

  auto content_str = json_to_chapter_text(response.text());

  static std::int32_t image_count = 1;
  std::vector<std::string> content;
  for (auto &line : klib::split_str(content_str, "\n")) {
    klib::trim(line);

    if (line.starts_with("[img")) {
      auto begin = line.find("https");
      if (begin == std::string::npos) {
        klib::warn("Invalid image URL: {}", line);
        continue;
      }

      auto end = line.find("[/img]");
      if (end == std::string::npos) {
        klib::warn("Invalid image URL: {}", line);
        continue;
      }

      auto image_url = line.substr(begin, end - begin);

      klib::Response image;
      try {
        image = http_get_rss(image_url);
      } catch (const klib::RuntimeError &err) {
        klib::warn("{}: {}", err.what(), line);
        continue;
      }

      auto image_name = kepub::num_to_str(image_count++) + ".jpg";
      image.save_to_file(image_name);

      line = "[IMAGE] " + image_name;
    }

    kepub::push_back(content, line);
  }

  return content;
}

}  // namespace

int main(int argc, const char *argv[]) try {
  CLI::App app;
  app.set_version_flag("-v,--version", kepub::version_str());

  std::string book_id;
  app.add_option("book-id", book_id, "The book id of the book to be downloaded")
      ->required();

  CLI11_PARSE(app, argc, argv)

  kepub::check_is_book_id(book_id);

  if (!show_user_info()) {
    auto login_name = kepub::get_login_name();
    auto password = kepub::get_password();
    login(login_name, password);
    klib::cleanse(password);
  }

  auto book_info = get_book_info(book_id);

  klib::info("Start getting chapter information");
  auto volumes = get_volume_chapter(book_id);

  std::int32_t chapter_count = 0;
  for (const auto &volume : volumes) {
    chapter_count += std::size(volume.chapters_);
  }

  klib::info("Start downloading novel content");
  kepub::ProgressBar bar(book_info.name_, chapter_count);
  for (auto &volume : volumes) {
    for (auto &chapter : volume.chapters_) {
      bar.set_postfix_text(chapter.title_);
      chapter.texts_ = get_content(chapter.id_);
      bar.tick();
    }
  }

  kepub::generate_txt(book_info, volumes);
  klib::info("Novel '{}' download completed", book_info.name_);
} catch (const klib::Exception &err) {
  klib::error(err.what());
} catch (const std::exception &err) {
  klib::error(err.what());
} catch (...) {
  klib::error("Unknown exception");
}
