// OBS Studio の中で動き、導入したプラグインを確かめる検査用のモジュール。配布物には含めない。
// 環境変数 LTS_PROBE_RESULT_DIR と LTS_PROBE_PHASE があるときだけ読み込まれる。
// - setup: 登録と表示名、Dock の部品、Dock からの Play と ON/OFF、Dock を閉じても再生が続くこと、
//   読み込みのエラーの表示、ソースの名前の変更と削除への Dock の追従、5 分のステム 4 本でのメモリと CPU を確かめる。
//   検査用のソースの設定と ON/OFF を残して OBS を閉じる
// - restore: 再起動後に、設定と ON/OFF が残り、停止した状態で始まり、Play で残したトラックだけが鳴ることを確かめる
// 結果を result.json に書き、OBS を閉じる。

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QComboBox>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()

namespace {

constexpr const char *kModuleName = "loop-track-switcher";
constexpr const char *kSourceId = "loop_track_switcher_source";
constexpr const char *kDockName = "loop-track-switcher-dock";
constexpr const char *kMainName = "LTS probe";
constexpr const char *kErrorName = "LTS probe errors";
constexpr const char *kRenamedErrorName = "LTS probe errors renamed";
constexpr const char *kLongName = "LTS probe long";
constexpr uint32_t kRate = 48000;
constexpr double kFrequencies[4] = {300.0, 500.0, 700.0, 900.0};
constexpr double kAmplitude = 0.2;
constexpr double kPi = 3.14159265358979323846;

QString result_dir;
QString phase;
QJsonObject evidence;
QJsonArray failures;

QMainWindow *main_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

void check(bool condition, const QString &what)
{
	if (!condition)
		failures.append(what);
}

void finish()
{
	const bool passed = failures.isEmpty();
	QJsonObject result{{"passed", passed},
			   {"failures", failures},
			   {"obs", obs_get_version_string()},
			   {"phase", phase},
			   {"evidence", evidence}};
	QFile file(result_dir + "/result.json");
	const auto json = QJsonDocument(result).toJson();
	if (file.open(QIODevice::WriteOnly))
		file.write(json);
	file.close();
	blog(passed ? LOG_INFO : LOG_ERROR, "LTS_PROBE %s phase=%s", passed ? "PASS" : "FAIL",
	     phase.toUtf8().constData());
	auto *window = main_window();
	QTimer::singleShot(0, window, [window] { window->close(); });
}

// Runs the steps one after another on the UI thread. Each step returns the delay before the next one in ms.
std::vector<std::function<int()>> steps;
size_t next_step = 0;

void run_next()
{
	if (next_step >= steps.size()) {
		finish();
		return;
	}
	const int delay = steps[next_step++]();
	QTimer::singleShot(delay, main_window(), run_next);
}

bool write_wav(const QString &path, double frequency, double seconds, uint16_t channels)
{
	const uint32_t frames = static_cast<uint32_t>(seconds * kRate);
	const uint32_t data_size = frames * channels * 2;
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;
	QByteArray header;
	const auto put32 = [&header](uint32_t v) {
		for (int i = 0; i < 4; ++i)
			header.append(static_cast<char>((v >> (8 * i)) & 0xFF));
	};
	const auto put16 = [&header](uint16_t v) {
		header.append(static_cast<char>(v & 0xFF));
		header.append(static_cast<char>(v >> 8));
	};
	header.append("RIFF");
	put32(36 + data_size);
	header.append("WAVEfmt ");
	put32(16);
	put16(1);
	put16(channels);
	put32(kRate);
	put32(kRate * channels * 2);
	put16(static_cast<uint16_t>(channels * 2));
	put16(16);
	header.append("data");
	put32(data_size);
	file.write(header);
	// Written in blocks of one second so that a 5-minute file needs little memory here
	std::vector<int16_t> block(static_cast<size_t>(kRate) * channels);
	for (uint32_t start = 0; start < frames; start += kRate) {
		const uint32_t count = std::min(kRate, frames - start);
		for (uint32_t i = 0; i < count; ++i) {
			const double v = kAmplitude * std::sin(2.0 * kPi * frequency * (start + i) / kRate);
			const auto s = static_cast<int16_t>(std::lround(v * 32767.0));
			for (uint16_t c = 0; c < channels; ++c)
				block[static_cast<size_t>(i) * channels + c] = s;
		}
		file.write(reinterpret_cast<const char *>(block.data()), count * channels * 2);
	}
	return file.error() == QFileDevice::NoError;
}

struct Capture {
	std::mutex mutex;
	std::vector<float> left;
};
Capture capture;

void capture_audio(void *, obs_source_t *, const struct audio_data *data, bool)
{
	std::lock_guard<std::mutex> lock(capture.mutex);
	const auto *left = reinterpret_cast<const float *>(data->data[0]);
	if (left)
		capture.left.insert(capture.left.end(), left, left + data->frames);
}

void clear_capture()
{
	std::lock_guard<std::mutex> lock(capture.mutex);
	capture.left.clear();
}

std::vector<float> take_capture()
{
	std::lock_guard<std::mutex> lock(capture.mutex);
	return capture.left;
}

double tone_amplitude(const std::vector<float> &samples, double frequency)
{
	if (samples.empty())
		return 0.0;
	const double coefficient = 2.0 * std::cos(2.0 * kPi * frequency / kRate);
	double s1 = 0.0, s2 = 0.0;
	for (float x : samples) {
		const double s0 = double(x) + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
	return 2.0 * std::sqrt(std::max(power, 0.0)) / double(samples.size());
}

// Records the amplitude of each track's frequency and checks which tracks are heard
void check_tones(const QString &label, const bool expected[4])
{
	const std::vector<float> samples = take_capture();
	QJsonArray amplitudes;
	for (int t = 0; t < 4; t++) {
		const double a = tone_amplitude(samples, kFrequencies[t]);
		amplitudes.append(std::round(a * 10000.0) / 10000.0);
		if (expected[t])
			check(std::fabs(a - kAmplitude) < kAmplitude * 0.1,
			      QString("%1: track %2 should be heard (%3)").arg(label).arg(t + 1).arg(a));
		else
			check(a < kAmplitude * 0.02,
			      QString("%1: track %2 should be silent (%3)").arg(label).arg(t + 1).arg(a));
	}
	evidence.insert(label,
			QJsonObject{{"frames", static_cast<qint64>(samples.size())}, {"amplitudes", amplitudes}});
}

struct State {
	long long transport = -1;
	double position = 0.0;
	long long loop_status = -1;
	long long enabled_mask = -1;
	int ready = 0;
	int errors = 0;
};

State get_state(const char *name)
{
	State state;
	obs_source_t *source = obs_get_source_by_name(name);
	if (!source)
		return state;
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_state", &cd);
	state.transport = calldata_int(&cd, "transport");
	state.position = calldata_float(&cd, "position");
	state.loop_status = calldata_int(&cd, "loop_status");
	state.enabled_mask = calldata_int(&cd, "enabled_mask");
	for (int t = 0; t < 4; t++) {
		const long long s = calldata_int(&cd, ("state" + std::to_string(t)).c_str());
		state.ready += s == 2 ? 1 : 0;
		state.errors += s == 3 ? 1 : 0;
	}
	calldata_free(&cd);
	obs_source_release(source);
	return state;
}

QDockWidget *dock()
{
	return main_window()->findChild<QDockWidget *>(kDockName);
}

template<typename T> T *dock_child(const char *name)
{
	auto *d = dock();
	return d ? d->findChild<T *>(name) : nullptr;
}

bool select_in_dock(const char *source_name)
{
	auto *combo = dock_child<QComboBox>("source_combo");
	if (!combo)
		return false;
	const int index = combo->findText(QString::fromUtf8(source_name));
	if (index < 0)
		return false;
	combo->setCurrentIndex(index);
	return true;
}

QStringList combo_items()
{
	QStringList items;
	if (auto *combo = dock_child<QComboBox>("source_combo"))
		for (int i = 0; i < combo->count(); i++)
			items.append(combo->itemText(i));
	return items;
}

obs_source_t *create_in_scene(const char *name, obs_data_t *settings)
{
	obs_source_t *source = obs_source_create(kSourceId, name, settings, nullptr);
	obs_source_t *scene_source = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (scene && source)
		obs_scene_add(scene, source);
	obs_source_release(scene_source);
	return source;
}

double cpu_percent(os_cpu_usage_info_t *info, int ms)
{
	os_cpu_usage_info_query(info);
	const uint64_t until = os_gettime_ns() + static_cast<uint64_t>(ms) * 1000000ULL;
	while (os_gettime_ns() < until)
		os_sleep_ms(50);
	return os_cpu_usage_info_query(info);
}

double resident_mb()
{
	return static_cast<double>(os_get_proc_resident_size()) / (1024.0 * 1024.0);
}

QString wav_path(const char *name)
{
	return result_dir + "/" + name;
}

void setup_steps()
{
	steps.push_back([] {
		check(obs_get_module(kModuleName) != nullptr, "module is loaded");
		const char *display = obs_source_get_display_name(kSourceId);
		evidence.insert("display_name", QString::fromUtf8(display ? display : ""));
		check(display && QString::fromUtf8(display) == "Loop Track Switcher", "display name in en-US");
		check(dock() != nullptr, "dock exists");
		for (const char *name : {"source_combo", "play_button", "pause_button", "stop_button", "track_button_1",
					 "track_button_4", "status_label"})
			check(dock() && dock()->findChild<QWidget *>(name) != nullptr,
			      QString("dock has %1").arg(name));
		check(combo_items().size() == 1, "dock shows the placeholder without sources");

		bool written = true;
		for (int t = 0; t < 4; t++)
			written &= write_wav(wav_path(("track" + std::to_string(t + 1) + ".wav").c_str()),
					     kFrequencies[t], 2.0, 2);
		written &= write_wav(wav_path("mono.wav"), 440.0, 1.0, 1);
		check(written, "probe WAV files are written");

		obs_data_t *settings = obs_data_create();
		for (int t = 0; t < 4; t++)
			obs_data_set_string(
				settings, ("track" + std::to_string(t + 1) + "_path").c_str(),
				wav_path(("track" + std::to_string(t + 1) + ".wav").c_str()).toUtf8().constData());
		obs_data_set_double(settings, "loop_start", 0.5);
		obs_data_set_double(settings, "loop_end", 1.5);
		obs_source_t *source = create_in_scene(kMainName, settings);
		obs_data_release(settings);
		if (source) {
			obs_source_add_audio_capture_callback(source, capture_audio, nullptr);
			obs_source_release(source);
		}
		check(source != nullptr, "main probe source is created");
		return 1500;
	});
	steps.push_back([] {
		const State s = get_state(kMainName);
		check(s.ready == 4, "four tracks are loaded");
		check(s.loop_status == 0, "loop is valid");
		check(s.transport == 0, "a new source starts stopped");
		check(combo_items().contains(kMainName), "dock lists the new source");
		check(select_in_dock(kMainName), "dock selects the source");
		if (auto *play = dock_child<QPushButton>("play_button"))
			play->click();
		return 400;
	});
	steps.push_back([] {
		clear_capture();
		return 600;
	});
	steps.push_back([] {
		check(get_state(kMainName).transport == 1, "dock Play starts playback");
		const bool all[4] = {true, true, true, true};
		check_tones("dock_play_all_tracks", all);
		if (auto *b = dock_child<QPushButton>("track_button_1"))
			b->click();
		return 400;
	});
	steps.push_back([] {
		clear_capture();
		return 600;
	});
	steps.push_back([] {
		check((get_state(kMainName).enabled_mask & 1) == 0, "dock switch turns track 1 off");
		const bool without_first[4] = {false, true, true, true};
		check_tones("dock_track1_off", without_first);
		if (auto *d = dock())
			d->close();
		return 300;
	});
	steps.push_back([] {
		clear_capture();
		return 600;
	});
	steps.push_back([] {
		check(dock() && !dock()->isVisible(), "dock is hidden");
		check(get_state(kMainName).transport == 1, "playback continues with the dock closed");
		const bool without_first[4] = {false, true, true, true};
		check_tones("dock_closed", without_first);
		if (auto *d = dock())
			d->show();

		// Load errors: a mono file and a missing file
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "track1_path", wav_path("mono.wav").toUtf8().constData());
		obs_data_set_string(settings, "track2_path", wav_path("missing.wav").toUtf8().constData());
		obs_source_t *source = create_in_scene(kErrorName, settings);
		obs_data_release(settings);
		obs_source_release(source);
		return 1000;
	});
	steps.push_back([] {
		const State s = get_state(kErrorName);
		check(s.errors == 2, "two load errors are reported");
		check(s.loop_status == 1, "no track is loaded for the error source");
		check(select_in_dock(kErrorName), "dock selects the error source");
		return 300;
	});
	steps.push_back([] {
		auto *status = dock_child<QLabel>("status_label");
		const QString text = status ? status->text() : QString();
		evidence.insert("dock_error_text", text);
		check(text.contains("stereo") && text.contains("opened"), "dock shows both load errors");

		obs_source_t *source = obs_get_source_by_name(kErrorName);
		obs_properties_t *props = obs_source_properties(source);
		obs_property_t *status_prop = obs_properties_get(props, "status");
		const char *desc = status_prop ? obs_property_description(status_prop) : nullptr;
		evidence.insert("properties_status_text", QString::fromUtf8(desc ? desc : ""));
		check(desc && QString::fromUtf8(desc).contains("stereo"), "properties show the load error");
		obs_properties_destroy(props);

		obs_source_set_name(source, kRenamedErrorName);
		obs_source_release(source);
		return 500;
	});
	steps.push_back([] {
		check(combo_items().contains(kRenamedErrorName), "dock follows a rename");
		obs_source_t *source = obs_get_source_by_name(kRenamedErrorName);
		obs_source_remove(source);
		obs_source_release(source);
		return 800;
	});
	steps.push_back([] {
		const QStringList items = combo_items();
		evidence.insert("dock_items_after_remove", QJsonArray::fromStringList(items));
		check(!items.contains(kRenamedErrorName), "dock drops a removed source");
		check(select_in_dock(kMainName), "dock selects the main source again");

		// Memory and CPU with four 5-minute stems
		os_cpu_usage_info_t *info = os_cpu_usage_info_start();
		evidence.insert("cpu_percent_before_long", cpu_percent(info, 3000));
		os_cpu_usage_info_destroy(info);
		evidence.insert("resident_mb_before_long", resident_mb());
		bool written = true;
		for (int t = 0; t < 4; t++)
			written &= write_wav(wav_path(("long" + std::to_string(t + 1) + ".wav").c_str()),
					     kFrequencies[t], 300.0, 2);
		check(written, "four 5-minute WAV files are written");
		obs_data_t *settings = obs_data_create();
		for (int t = 0; t < 4; t++)
			obs_data_set_string(
				settings, ("track" + std::to_string(t + 1) + "_path").c_str(),
				wav_path(("long" + std::to_string(t + 1) + ".wav").c_str()).toUtf8().constData());
		const uint64_t start = os_gettime_ns();
		obs_source_t *source = create_in_scene(kLongName, settings);
		obs_data_release(settings);
		obs_source_release(source);
		// Waits on the UI thread only in this probe, to time the load
		const uint64_t deadline = start + 30000000000ULL;
		while (get_state(kLongName).ready < 4 && os_gettime_ns() < deadline)
			os_sleep_ms(20);
		evidence.insert("long_load_seconds", static_cast<double>(os_gettime_ns() - start) / 1e9);
		check(get_state(kLongName).ready == 4, "four 5-minute tracks are loaded");
		return 500;
	});
	steps.push_back([] {
		evidence.insert("resident_mb_with_long", resident_mb());
		obs_source_t *source = obs_get_source_by_name(kLongName);
		calldata_t cd = {};
		proc_handler_call(obs_source_get_proc_handler(source), "play", &cd);
		calldata_free(&cd);
		obs_source_release(source);
		os_cpu_usage_info_t *info = os_cpu_usage_info_start();
		evidence.insert("cpu_percent_playing_long", cpu_percent(info, 3000));
		os_cpu_usage_info_destroy(info);
		source = obs_get_source_by_name(kLongName);
		obs_source_remove(source);
		obs_source_release(source);
		return 2000;
	});
	steps.push_back([] {
		evidence.insert("resident_mb_after_remove_long", resident_mb());
		for (int t = 0; t < 4; t++)
			QFile::remove(wav_path(("long" + std::to_string(t + 1) + ".wav").c_str()));
		// Leave the main source paused with track 1 off, so that restore can check what is kept
		obs_source_t *source = obs_get_source_by_name(kMainName);
		calldata_t cd = {};
		proc_handler_call(obs_source_get_proc_handler(source), "pause", &cd);
		calldata_free(&cd);
		obs_source_release(source);
		return 500;
	});
}

void restore_steps()
{
	steps.push_back([] {
		const char *display = obs_source_get_display_name(kSourceId);
		evidence.insert("display_name", QString::fromUtf8(display ? display : ""));
		check(display && QString::fromUtf8(display) == QString::fromUtf8(u8"ループトラックスイッチャー"),
		      "display name in ja-JP");
		auto *play = dock_child<QPushButton>("play_button");
		check(play && play->text() == QString::fromUtf8(u8"再生"), "dock text in ja-JP");

		obs_source_t *source = obs_get_source_by_name(kMainName);
		check(source != nullptr, "main probe source is restored");
		if (source) {
			obs_data_t *settings = obs_source_get_settings(source);
			check(obs_data_get_double(settings, "loop_start") == 0.5 &&
				      obs_data_get_double(settings, "loop_end") == 1.5,
			      "loop settings are restored");
			check(QString::fromUtf8(obs_data_get_string(settings, "track3_path")).endsWith("track3.wav"),
			      "track paths are restored");
			obs_data_release(settings);
			obs_source_add_audio_capture_callback(source, capture_audio, nullptr);
			obs_source_release(source);
		}
		return 1500;
	});
	steps.push_back([] {
		const State s = get_state(kMainName);
		check(s.ready == 4, "four tracks are loaded after restart");
		check(s.transport == 0, "playback starts stopped after restart");
		check(s.enabled_mask == 0b1110, "track switches are restored (track 1 off)");
		evidence.insert("enabled_mask", s.enabled_mask);
		check(select_in_dock(kMainName), "dock selects the restored source");
		if (auto *play = dock_child<QPushButton>("play_button"))
			play->click();
		return 400;
	});
	steps.push_back([] {
		clear_capture();
		return 600;
	});
	steps.push_back([] {
		const bool without_first[4] = {false, true, true, true};
		check_tones("restored_play", without_first);
		if (auto *stop = dock_child<QPushButton>("stop_button"))
			stop->click();
		return 500;
	});
	steps.push_back([] {
		check(get_state(kMainName).transport == 0, "dock Stop stops playback");
		return 0;
	});
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;
	if (phase == "setup")
		setup_steps();
	else
		restore_steps();
	// Let the plugin add its dock first, which it also does on FINISHED_LOADING
	QTimer::singleShot(1000, main_window(), run_next);
}

} // namespace

bool obs_module_load(void)
{
	const char *dir = getenv("LTS_PROBE_RESULT_DIR");
	const char *p = getenv("LTS_PROBE_PHASE");
	if (!dir || !p)
		return false;
	result_dir = QString::fromLocal8Bit(dir);
	phase = QString::fromLocal8Bit(p);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	return true;
}
