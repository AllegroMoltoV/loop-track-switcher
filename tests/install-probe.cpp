// OBS Studio の中で動き、導入したプラグインを確かめる検査用のモジュール。配布物には含めない。
// 環境変数 LTS_PROBE_RESULT_DIR、LTS_PROBE_PHASE、LTS_PROBE_SONGS があるときだけ読み込まれる。
// LTS_PROBE_SONGS は .tmp/2026112514-make-test-songs.py が書いた検証用の曲のフォルダー。
//
// - setup: 登録と Dock の部品を確かめたあと、計画の作業 6 のシナリオ S1〜S7 を Dock の操作で通す。
//   各ステムの区間の頭にある目印の音 (3〜6 kHz) を出力から探して同期を確かめ、[!] の表示、メモリ、CPU を記録する。
//   S8 のために設定を残して OBS を閉じる
// - restore: S8。再起動後に設定が戻り、停止した状態で始まり、ベイクで変換を省くことを確かめる。
//   S4 で移動したファイルに [!] が出て、書き出し直したファイルが読み直されることを確かめる
// 結果を result.json に書き、OBS を閉じる。

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QSlider>
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
constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;
// Marker pips of the test songs: stem 1 to 4 at 3, 4, 5, 6 kHz, 10 ms long, at 0 s, 4 s, and 12 s
constexpr double kMarkers[4] = {3000.0, 4000.0, 5000.0, 6000.0};

QString result_dir;
QString phase;
QString songs;
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
	QFile file(result_dir + "/result-" + phase + ".json");
	if (file.open(QIODevice::WriteOnly))
		file.write(QJsonDocument(result).toJson());
	file.close();
	blog(passed ? LOG_INFO : LOG_ERROR, "LTS_PROBE %s phase=%s", passed ? "PASS" : "FAIL",
	     phase.toUtf8().constData());
	auto *window = main_window();
	QTimer::singleShot(0, window, [window] { window->close(); });
}

// Steps run one after another on the UI thread. Each returns the delay before the next one in ms.
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

// The second scene of scenario S6, created and released by the probe
obs_scene_t *scene2 = nullptr;

QString song(const QString &relative)
{
	return QDir(songs).filePath(relative);
}

// --- Audio capture ---

struct Capture {
	std::mutex mutex;
	std::vector<float> left;
	std::vector<float> right;
};
Capture capture;

void capture_audio(void *, obs_source_t *, const struct audio_data *data, bool)
{
	std::lock_guard<std::mutex> lock(capture.mutex);
	const auto *left = reinterpret_cast<const float *>(data->data[0]);
	const auto *right = reinterpret_cast<const float *>(data->data[1]);
	if (left && right) {
		capture.left.insert(capture.left.end(), left, left + data->frames);
		capture.right.insert(capture.right.end(), right, right + data->frames);
	}
}

void clear_capture()
{
	std::lock_guard<std::mutex> lock(capture.mutex);
	capture.left.clear();
	capture.right.clear();
}

std::vector<float> take_mono()
{
	std::lock_guard<std::mutex> lock(capture.mutex);
	std::vector<float> mono(capture.left.size());
	for (size_t i = 0; i < mono.size(); i++)
		mono[i] = 0.5f * (capture.left[i] + capture.right[i]);
	return mono;
}

double tone(const std::vector<float> &samples, size_t from, size_t count, double frequency)
{
	const double coefficient = 2.0 * std::cos(2.0 * kPi * frequency / kRate);
	double s1 = 0.0, s2 = 0.0;
	for (size_t i = from; i < std::min(from + count, samples.size()); i++) {
		const double s0 = double(samples[i]) + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return 2.0 * std::sqrt(std::max(s1 * s1 + s2 * s2 - coefficient * s1 * s2, 0.0)) / double(count);
}

// Finds the 5 ms block where each stem's marker is loudest, and checks that the markers of the given stems are
// at most one block apart. The stems were mixed from one shared position, so their markers must coincide.
void check_markers_together(const QString &label, const std::vector<int> &stems)
{
	const std::vector<float> mono = take_mono();
	constexpr size_t block = 240;
	QJsonArray peaks;
	int first = -1, last = -1;
	for (int stem : stems) {
		double best = 0.0;
		int at = -1;
		for (size_t i = 0; i + block <= mono.size(); i += block) {
			const double a = tone(mono, i, block, kMarkers[stem]);
			if (a > best) {
				best = a;
				at = static_cast<int>(i / block);
			}
		}
		peaks.append(QJsonObject{{"stem", stem + 1}, {"block", at}, {"amplitude", best}});
		check(best > 0.005, QString("%1: marker of stem %2 is found (%3)").arg(label).arg(stem + 1).arg(best));
		first = first < 0 ? at : std::min(first, at);
		last = std::max(last, at);
	}
	evidence.insert(label, QJsonObject{{"frames", static_cast<qint64>(mono.size())}, {"peaks", peaks}});
	check(last - first <= 1,
	      QString("%1: markers of all stems coincide (blocks %2 to %3)").arg(label).arg(first).arg(last));
}

// --- Source state through the proc handler ---

struct State {
	long long transport = -1;
	long long position = 0;
	long long song_end = 0;
	long long requested_start = 0;
	long long requested_end = 0;
	long long loop_status = -1;
	bool loop_enabled = false;
	bool crossfade = false;
	long long enabled_mask = 0;
	long long memory_bytes = 0;
	int ready = 0;
	int errors = 0;
	long long frames[4] = {};
	long long full_frames[4] = {};
	long long channels[4] = {};
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
	state.position = calldata_int(&cd, "position");
	state.song_end = calldata_int(&cd, "song_end");
	state.requested_start = calldata_int(&cd, "requested_start");
	state.requested_end = calldata_int(&cd, "requested_end");
	state.loop_status = calldata_int(&cd, "loop_status");
	state.loop_enabled = calldata_bool(&cd, "loop_enabled");
	state.crossfade = calldata_bool(&cd, "crossfade");
	state.enabled_mask = calldata_int(&cd, "enabled_mask");
	state.memory_bytes = calldata_int(&cd, "memory_bytes");
	for (int t = 0; t < 4; t++) {
		const std::string i = std::to_string(t);
		const long long s = calldata_int(&cd, ("state" + i).c_str());
		state.ready += s == 2 ? 1 : 0;
		state.errors += s == 3 ? 1 : 0;
		state.frames[t] = calldata_int(&cd, ("frames" + i).c_str());
		state.full_frames[t] = calldata_int(&cd, ("full_frames" + i).c_str());
		state.channels[t] = calldata_int(&cd, ("channels" + i).c_str());
	}
	calldata_free(&cd);
	obs_source_release(source);
	return state;
}

obs_source_t *create_in_scene(const char *name, const QStringList &paths, obs_source_t *scene_source = nullptr)
{
	obs_data_t *settings = obs_data_create();
	for (int t = 0; t < paths.size() && t < 4; t++)
		obs_data_set_string(settings, ("track" + std::to_string(t + 1) + "_path").c_str(),
				    paths[t].toUtf8().constData());
	obs_source_t *source = obs_source_create(kSourceId, name, settings, nullptr);
	obs_data_release(settings);
	obs_source_t *target = scene_source ? obs_source_get_ref(scene_source) : obs_frontend_get_current_scene();
	if (obs_scene_t *scene = obs_scene_from_source(target))
		obs_scene_add(scene, source);
	obs_source_release(target);
	return source;
}

void remove_source(const char *name)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (source) {
		obs_source_remove(source);
		obs_source_release(source);
	}
}

// Waits on the UI thread, for the loading steps of this probe only
void wait_loaded(const char *name, int expected, int timeout_ms)
{
	const uint64_t deadline = os_gettime_ns() + static_cast<uint64_t>(timeout_ms) * 1000000ULL;
	while (os_gettime_ns() < deadline) {
		const State s = get_state(name);
		if (s.ready + s.errors >= expected)
			return;
		os_sleep_ms(20);
	}
}

// --- Dock ---

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

void click(const char *name)
{
	if (auto *b = dock_child<QAbstractButton>(name))
		b->click();
	else
		check(false, QString("dock button %1 exists").arg(name));
}

bool checked(const char *name)
{
	auto *b = dock_child<QAbstractButton>(name);
	return b && b->isChecked();
}

// Enters a value in a number field of the dock the way a user does: type, then finish editing
void enter_value(const char *spin_name, double value)
{
	auto *spin = dock_child<QDoubleSpinBox>(spin_name);
	if (!spin) {
		check(false, QString("dock field %1 exists").arg(spin_name));
		return;
	}
	spin->setValue(value);
	emit spin->editingFinished();
}

QStringList visible_warnings()
{
	QStringList list;
	for (const char *name : {"track_warning_1", "track_warning_2", "track_warning_3", "track_warning_4",
				 "loop_start_warning", "loop_end_warning"}) {
		auto *label = dock_child<QLabel>(name);
		if (label && label->isVisibleTo(dock()))
			list << QString("%1: %2").arg(name, label->toolTip());
	}
	return list;
}

bool warning_visible(const char *name)
{
	auto *label = dock_child<QLabel>(name);
	return label && label->isVisibleTo(dock());
}

void seek_seconds(const char *name, double seconds)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (!source)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "frames", std::llround(seconds * kRate));
	proc_handler_call(obs_source_get_proc_handler(source), "seek", &cd);
	calldata_free(&cd);
	obs_source_release(source);
}

void attach_capture(const char *name)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (source) {
		obs_source_add_audio_capture_callback(source, capture_audio, nullptr);
		obs_source_release(source);
	}
}

void detach_capture(const char *name)
{
	obs_source_t *source = obs_get_source_by_name(name);
	if (source) {
		obs_source_remove_audio_capture_callback(source, capture_audio, nullptr);
		obs_source_release(source);
	}
}

double resident_mb()
{
	return static_cast<double>(os_get_proc_resident_size()) / (1024.0 * 1024.0);
}

double cpu_percent(int ms)
{
	os_cpu_usage_info_t *info = os_cpu_usage_info_start();
	os_cpu_usage_info_query(info);
	const uint64_t until = os_gettime_ns() + static_cast<uint64_t>(ms) * 1000000ULL;
	while (os_gettime_ns() < until)
		os_sleep_ms(50);
	const double value = os_cpu_usage_info_query(info);
	os_cpu_usage_info_destroy(info);
	return value;
}

QStringList stems(const QString &folder)
{
	return {song(folder + "/1-pad.wav"), song(folder + "/2-bass.wav"), song(folder + "/3-drums.wav"),
		song(folder + "/4-lead.wav")};
}

// The work folder of the probe for files it moves and rewrites (S4, S5)
QString work(const QString &relative)
{
	return QDir(result_dir).filePath("work/" + relative);
}

// --- Setup phase ---

void setup_steps()
{
	// Basics
	steps.push_back([] {
		check(obs_get_module(kModuleName) != nullptr, "module is loaded");
		check(QString::fromUtf8(obs_source_get_display_name(kSourceId)) == "Loop Track Switcher",
		      "display name in en-US");
		check(dock() != nullptr, "dock exists");
		for (const char *name : {"source_combo", "help_button", "play_button", "pause_button", "stop_button",
					 "loop_button", "seek_bar", "time_label", "loop_start_slider",
					 "loop_start_spin", "loop_end_slider", "loop_end_spin", "track_button_1",
					 "track_button_4", "track_warning_1", "unit_combo", "crossfade_check"})
			check(dock() && dock()->findChild<QWidget *>(name) != nullptr,
			      QString("dock has %1").arg(name));
		auto *help = dock_child<QWidget>("help_button");
		check(help && help->toolTip().contains("source"), "the help button explains creating a source first");
		evidence.insert("resident_mb_start", resident_mb());
		evidence.insert("cpu_percent_idle", cpu_percent(2000));
		return 200;
	});

	// S1: stems exported from a DAW as MP3 44.1 kHz
	steps.push_back([] {
		QStringList paths;
		for (const char *n : {"1-pad", "2-bass", "3-drums", "4-lead"})
			paths << song(QString("daw-mp3/%1.mp3").arg(n));
		obs_source_t *s = create_in_scene("S1 DAW", paths);
		obs_source_release(s);
		const uint64_t start = os_gettime_ns();
		wait_loaded("S1 DAW", 4, 30000);
		evidence.insert("s1_first_load_ms", static_cast<double>(os_gettime_ns() - start) / 1e6);
		const State st = get_state("S1 DAW");
		check(st.ready == 4, "S1: four MP3 stems are loaded");
		check(st.frames[0] == 768000 && st.frames[3] == 768000,
		      QString("S1: MP3 stems are exactly 16 s after removing the encoder delay (%1)").arg(st.frames[0]));
		return 400;
	});
	steps.push_back([] {
		// The dock learns about new sources through queued signals, so it is used one step after the creation
		check(select_in_dock("S1 DAW"), "S1: the dock selects the source");
		return 200;
	});
	steps.push_back([] {
		enter_value("loop_start_spin", 4.0);
		enter_value("loop_end_spin", 12.0);
		return 300;
	});
	steps.push_back([] {
		const State st = get_state("S1 DAW");
		check(st.requested_start == 192000 && st.requested_end == 576000,
		      QString("S1: the dock sets the loop to 4 s - 12 s (%1 - %2)")
			      .arg(st.requested_start)
			      .arg(st.requested_end));
		check(visible_warnings().isEmpty(),
		      "S1: aligned stems show no [!] (" + visible_warnings().join("; ") + ")");
		attach_capture("S1 DAW");
		seek_seconds("S1 DAW", 3.8);
		click("play_button");
		clear_capture();
		return 600;
	});
	steps.push_back([] {
		check(checked("play_button") && !checked("stop_button"),
		      "S1: the play button is highlighted while playing");
		check_markers_together("s1_markers_at_loop_start", {0, 1, 2, 3});
		// Loop off near the loop end: goes into the outro and stops at the song end
		seek_seconds("S1 DAW", 11.6);
		return 100;
	});
	steps.push_back([] {
		click("loop_button");
		check(!get_state("S1 DAW").loop_enabled, "S1: the loop button turns the loop off");
		clear_capture();
		return 700;
	});
	steps.push_back([] {
		check_markers_together("s1_markers_at_outro", {0, 1, 2, 3});
		return 4500;
	});
	steps.push_back([] {
		const State st = get_state("S1 DAW");
		check(st.transport == 0 && st.position == 0, "S1: playback stops at the song end after the outro");
		check(!st.loop_enabled, "S1: stopping does not turn the loop back on");
		check(checked("stop_button"), "S1: the stop button is highlighted after the song end");
		detach_capture("S1 DAW");
		return 100;
	});

	// S2: mixed formats, rates, and channels
	steps.push_back([] {
		obs_source_t *s = create_in_scene("S2 mixed", {song(QString::fromUtf8(u8"chaos/1 パッド 和音.mp3")),
							       song("chaos/2-bass-mono.flac"),
							       song(QString::fromUtf8(u8"chaos/3 ドラム.ogg")),
							       song("chaos/4-lead-96k-24bit.wav")});
		obs_source_release(s);
		wait_loaded("S2 mixed", 4, 30000);
		const State st = get_state("S2 mixed");
		check(st.ready == 4, "S2: MP3, mono FLAC, Ogg, and 96 kHz 24-bit WAV are loaded");
		bool same = true;
		for (int t = 0; t < 4; t++)
			same = same && st.frames[t] == 768000;
		check(same, "S2: every format becomes 16 s at 48 kHz");
		return 400;
	});
	steps.push_back([] {
		check(select_in_dock("S2 mixed"), "S2: the dock selects the source");
		return 300;
	});
	steps.push_back([] {
		check(visible_warnings().isEmpty(),
		      "S2: mixed formats of equal length show no [!] (" + visible_warnings().join("; ") + ")");
		enter_value("loop_start_spin", 4.0);
		enter_value("loop_end_spin", 12.0);
		attach_capture("S2 mixed");
		seek_seconds("S2 mixed", 3.8);
		click("play_button");
		clear_capture();
		return 600;
	});
	steps.push_back([] {
		check_markers_together("s2_markers_at_loop_start", {0, 1, 2, 3});
		click("stop_button");
		detach_capture("S2 mixed");
		return 300;
	});

	// Misaligned lengths
	steps.push_back([] {
		obs_source_t *s = create_in_scene("Misaligned", stems("misaligned"));
		obs_source_release(s);
		wait_loaded("Misaligned", 4, 30000);
		return 400;
	});
	steps.push_back([] {
		check(select_in_dock("Misaligned"), "misaligned: the dock selects the source");
		return 300;
	});
	steps.push_back([] {
		const State st = get_state("Misaligned");
		check(st.song_end == 864000, "misaligned: the song end is the longest track (18 s)");
		check(!warning_visible("track_warning_1") && warning_visible("track_warning_2") &&
			      warning_visible("track_warning_3") && warning_visible("track_warning_4"),
		      "misaligned: [!] appears next to the tracks shorter than the song");
		auto *label = dock_child<QLabel>("track_warning_3");
		evidence.insert("misaligned_tooltip", label ? label->toolTip() : QString());
		// A picture of the dock with warnings, for the author to look at
		if (auto *d = dock())
			d->grab().save(result_dir + "/dock-misaligned.png");
		check(label && label->toolTip().contains("0:14.0") && label->toolTip().contains("0:18.0"),
		      "misaligned: the tooltip gives the track end and the song end");
		click("play_button");
		return 500;
	});
	steps.push_back([] {
		check(get_state("Misaligned").transport == 1, "misaligned: tracks of different lengths still play");
		click("stop_button");
		return 300;
	});

	// S3 and memory: a 30-minute file, and four distinct long files
	steps.push_back([] {
		QDir().mkpath(work("long"));
		for (int i = 1; i <= 4; i++)
			QFile::copy(song("chaos/x-30min-set.mp3"), work(QString("long/set-%1.mp3").arg(i)));
		evidence.insert("resident_mb_before_long", resident_mb());
		QStringList paths = stems("aligned");
		paths[0] = song("chaos/x-30min-set.mp3");
		obs_source_t *s = create_in_scene("S3 long", paths);
		obs_source_release(s);
		wait_loaded("S3 long", 4, 120000);
		const State st = get_state("S3 long");
		check(st.frames[0] == 28800000, "S3: the 30-minute file is cut at 10 minutes");
		check(st.full_frames[0] > 86000000, "S3: the original length is kept for the warning");
		return 400;
	});
	steps.push_back([] {
		check(select_in_dock("S3 long"), "S3: the dock selects the source");
		return 300;
	});
	steps.push_back([] {
		check(warning_visible("track_warning_1"), "S3: [!] appears next to the trimmed track");
		auto *label = dock_child<QLabel>("track_warning_1");
		evidence.insert("trim_tooltip", label ? label->toolTip() : QString());
		check(label && label->toolTip().contains("10:00.0") && label->toolTip().contains("30:00"),
		      "S3: the tooltip gives the cut point and the original length");
		click("play_button");
		evidence.insert("cpu_percent_playing_s3", cpu_percent(3000));
		click("stop_button");
		remove_source("S3 long");
		return 1500;
	});
	steps.push_back([] {
		// Peak memory while baking four distinct 10-minute tracks, and the memory after loading
		const double before = resident_mb();
		QStringList paths;
		for (int i = 1; i <= 4; i++)
			paths << work(QString("long/set-%1.mp3").arg(i));
		obs_source_t *s = create_in_scene("Memory max", paths);
		obs_source_release(s);
		double peak = before;
		const uint64_t start = os_gettime_ns();
		const uint64_t deadline = start + 240000000000ULL;
		while (os_gettime_ns() < deadline) {
			peak = std::max(peak, resident_mb());
			if (get_state("Memory max").ready == 4)
				break;
			os_sleep_ms(50);
		}
		const double after = resident_mb();
		evidence.insert("memory_max_first_load_ms", static_cast<double>(os_gettime_ns() - start) / 1e6);
		evidence.insert("memory_max_resident_before_mb", before);
		evidence.insert("memory_max_resident_peak_mb", peak);
		evidence.insert("memory_max_resident_after_mb", after);
		const State st = get_state("Memory max");
		evidence.insert("memory_max_reported_mb", static_cast<double>(st.memory_bytes) / (1024.0 * 1024.0));
		check(st.ready == 4, "memory: four distinct 10-minute tracks are loaded");
		check(after - before < 520.0,
		      QString("memory: the increase stays near the estimate of 440 MiB (%1 MB)").arg(after - before));
		check(peak - after < 120.0, QString("memory: baking does not need memory in proportion to the length "
						    "(peak %1 MB above the final)")
						    .arg(peak - after));
		click("stop_button");
		return 200;
	});
	steps.push_back([] {
		// S6: the same files in another source, in another scene, share the memory
		scene2 = obs_scene_create("Scene 2");
		QStringList paths;
		for (int i = 1; i <= 4; i++)
			paths << work(QString("long/set-%1.mp3").arg(i));
		const double before = resident_mb();
		obs_source_t *s = create_in_scene("Memory shared", paths, obs_scene_get_source(scene2));
		obs_source_release(s);
		wait_loaded("Memory shared", 4, 60000);
		evidence.insert("shared_increase_mb", resident_mb() - before);
		check(resident_mb() - before < 40.0, "S6: a second source with the same files shares the memory");
		return 400;
	});
	steps.push_back([] {
		check(select_in_dock("Memory shared") && select_in_dock("Memory max"),
		      "S6: the dock switches between the two sources");
		evidence.insert("resident_mb_before_remove", resident_mb());
		remove_source("Memory shared");
		remove_source("Memory max");
		// The probe's own scene is removed and released, so that it holds no source at exit
		obs_source_remove(obs_scene_get_source(scene2));
		obs_scene_release(scene2);
		scene2 = nullptr;
		// Measured after the event loop has run, because OBS releases some references from queued UI events
		return 4000;
	});
	steps.push_back([] {
		const double released = evidence.value("resident_mb_before_remove").toDouble() - resident_mb();
		evidence.insert("memory_released_mb", released);
		check(released > 300.0,
		      QString("memory: removing the sources releases the tracks (%1 MB)").arg(released));
		return 200;
	});

	// Broken files, 5.1, and a loop end beyond the song
	steps.push_back([] {
		obs_source_t *s =
			create_in_scene("Broken", {song("chaos/x-broken-cut.mp3"), song("chaos/x-broken-random.ogg"),
						   song("chaos/x-pad-5.1.flac"), song("aligned/4-lead.wav")});
		obs_source_release(s);
		wait_loaded("Broken", 4, 30000);
		return 400;
	});
	steps.push_back([] {
		check(select_in_dock("Broken"), "broken: the dock selects the source");
		return 300;
	});
	steps.push_back([] {
		const State st = get_state("Broken");
		evidence.insert("broken_warnings", QJsonArray::fromStringList(visible_warnings()));
		check(st.errors >= 1 && warning_visible("track_warning_2"),
		      "broken: a random Ogg file shows [!] and does not crash");
		check(st.channels[2] == 6 && warning_visible("track_warning_3"),
		      "broken: a 5.1 FLAC shows [!] for the channels");
		check(st.ready >= 2, "broken: the other tracks still load");
		obs_source_t *source = obs_get_source_by_name("Broken");
		calldata_t cd = {};
		calldata_set_int(&cd, "start", 0);
		calldata_set_int(&cd, "end", st.song_end + 48000);
		proc_handler_call(obs_source_get_proc_handler(source), "set_loop_range", &cd);
		calldata_free(&cd);
		obs_source_release(source);
		return 300;
	});
	steps.push_back([] {
		check(warning_visible("loop_end_warning"), "a loop end beyond the song shows [!] next to the loop end");
		remove_source("Broken");
		return 300;
	});

	// S4 and S5: files in a deep folder with spaces and Japanese, to be moved and rewritten before restore
	steps.push_back([] {
		const QString deep = work(QString::fromUtf8(u8"素材 フォルダー/配信 BGM/曲 1"));
		QDir().mkpath(deep);
		QFile::copy(song("daw-mp3/1-pad.mp3"), deep + QString::fromUtf8(u8"/パッド.mp3"));
		QFile::copy(song("daw-mp3/2-bass.mp3"), deep + QString::fromUtf8(u8"/ベース.mp3"));
		QFile::copy(song("aligned/3-drums.wav"), deep + QString::fromUtf8(u8"/ドラム.wav"));
		QFile::copy(song("aligned/4-lead.wav"), deep + QString::fromUtf8(u8"/リード.wav"));
		obs_source_t *s = create_in_scene("S4 moving", {deep + QString::fromUtf8(u8"/パッド.mp3"),
								deep + QString::fromUtf8(u8"/ベース.mp3"),
								deep + QString::fromUtf8(u8"/ドラム.wav"),
								deep + QString::fromUtf8(u8"/リード.wav")});
		obs_source_release(s);
		wait_loaded("S4 moving", 4, 30000);
		check(get_state("S4 moving").ready == 4,
		      "S5: files in a deep folder with spaces and Japanese names load");
		return 200;
	});

	// S7: operations during a stream
	steps.push_back([] {
		select_in_dock("S1 DAW");
		click("loop_button"); // back on
		click("play_button");
		return 400;
	});
	steps.push_back([] {
		for (int i = 0; i < 24; i++) {
			click(QString("track_button_%1").arg(i % 4 + 1).toUtf8().constData());
			os_sleep_ms(15);
		}
		for (int i = 0; i < 10; i++) {
			seek_seconds("S1 DAW", 4.0 + i * 0.7);
			os_sleep_ms(30);
		}
		if (auto *d = dock())
			d->close();
		return 600;
	});
	steps.push_back([] {
		const State st = get_state("S1 DAW");
		check(st.transport == 1, "S7: playback continues after rapid switching, seeking, and closing the dock");
		check(st.enabled_mask == 0b1111, "S7: an even number of clicks per track leaves every track on");
		if (auto *d = dock())
			d->show();
		// Pause in the outro and resume
		click("loop_button");
		seek_seconds("S1 DAW", 12.5);
		return 400;
	});
	steps.push_back([] {
		click("pause_button");
		return 400;
	});
	steps.push_back([] {
		const State paused = get_state("S1 DAW");
		check(paused.transport == 2 && paused.position > 600000, "S7: pause in the outro keeps the position");
		check(checked("pause_button"), "S7: the pause button is highlighted while paused");
		click("play_button");
		return 4000;
	});
	steps.push_back([] {
		check(get_state("S1 DAW").transport == 0, "S7: resuming in the outro plays to the song end and stops");
		// Leave settings for S8: S1 with the loop off, the crossfade off, track 3 off, and the unit in samples
		click("track_button_3");
		if (auto *c = dock_child<QCheckBox>("crossfade_check"))
			c->click();
		if (auto *u = dock_child<QComboBox>("unit_combo"))
			u->setCurrentIndex(2);
		evidence.insert("cpu_percent_end", cpu_percent(2000));
		evidence.insert("resident_mb_end", resident_mb());
		return 300;
	});
}

// --- Restore phase (S8 and the rest of S4) ---

void restore_steps()
{
	steps.push_back([] {
		check(QString::fromUtf8(obs_source_get_display_name(kSourceId)) ==
			      QString::fromUtf8(u8"ループトラックスイッチャー"),
		      "S8: display name in ja-JP");
		auto *play = dock_child<QPushButton>("play_button");
		check(play && play->text() == QString::fromUtf8(u8"再生"), "S8: dock text in ja-JP");
		wait_loaded("S1 DAW", 4, 30000);
		wait_loaded("S2 mixed", 4, 30000);
		wait_loaded("S4 moving", 4, 30000);
		return 500;
	});
	steps.push_back([] {
		const State s1 = get_state("S1 DAW");
		check(s1.ready == 4, "S8: S1 is loaded after the restart");
		check(s1.transport == 0, "S8: playback starts stopped");
		check(s1.requested_start == 192000 && s1.requested_end == 576000, "S8: the loop range is restored");
		check(!s1.loop_enabled, "S8: the loop switch is restored (off)");
		check(!s1.crossfade, "S8: the crossfade switch is restored (off)");
		check(s1.enabled_mask == 0b1011, "S8: track switches are restored (track 3 off)");
		auto *unit = dock_child<QComboBox>("unit_combo");
		check(unit && unit->currentIndex() == 2, "S8: the unit of the dock is restored (samples)");

		const State s4 = get_state("S4 moving");
		evidence.insert("s4_errors", s4.errors);
		check(s4.errors == 1, "S4: the moved file shows an error after the restart");
		check(s4.ready == 3, "S4: the rewritten file is loaded again");
		select_in_dock("S4 moving");
		return 300;
	});
	steps.push_back([] {
		check(warning_visible("track_warning_1"), "S4: [!] appears next to the moved file");
		select_in_dock("S1 DAW");
		click("play_button");
		return 600;
	});
	steps.push_back([] {
		check(get_state("S1 DAW").transport == 1, "S8: the restored source plays");
		click("stop_button");
		return 200;
	});
}

// --- CPU phase: compares the CPU use of OBS with no source, five media sources, and five loop track sources ---

void cpu_steps()
{
	auto measure = [](const char *key) {
		QJsonArray values;
		for (int i = 0; i < 3; i++)
			values.append(cpu_percent(5000));
		evidence.insert(key, values);
	};
	steps.push_back([measure] {
		measure("cpu_no_source");
		for (int i = 1; i <= 5; i++) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_string(settings, "local_file", song("aligned/1-pad.wav").toUtf8().constData());
			obs_data_set_bool(settings, "looping", true);
			obs_source_t *media = obs_source_create(
				"ffmpeg_source", QString("Media %1").arg(i).toUtf8().constData(), settings, nullptr);
			obs_data_release(settings);
			obs_source_t *scene_source = obs_frontend_get_current_scene();
			obs_scene_add(obs_scene_from_source(scene_source), media);
			obs_source_release(scene_source);
			obs_source_release(media);
		}
		return 2000;
	});
	steps.push_back([measure] {
		measure("cpu_five_media_sources_playing");
		for (int i = 1; i <= 5; i++)
			remove_source(QString("Media %1").arg(i).toUtf8().constData());
		for (int i = 1; i <= 5; i++) {
			obs_source_t *s =
				create_in_scene(QString("Loop %1").arg(i).toUtf8().constData(), stems("aligned"));
			obs_source_release(s);
		}
		return 3000;
	});
	steps.push_back([measure] {
		measure("cpu_five_loop_sources_stopped");
		for (int i = 1; i <= 5; i++) {
			obs_source_t *source = obs_get_source_by_name(QString("Loop %1").arg(i).toUtf8().constData());
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(source), "play", &cd);
			calldata_free(&cd);
			obs_source_release(source);
		}
		return 1000;
	});
	steps.push_back([measure] {
		measure("cpu_five_loop_sources_playing");
		return 0;
	});
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;
	if (phase == "setup")
		setup_steps();
	else if (phase == "cpu")
		cpu_steps();
	else
		restore_steps();
	// Let the plugin add its dock first, which it also does on FINISHED_LOADING
	QTimer::singleShot(1500, main_window(), run_next);
}

} // namespace

bool obs_module_load(void)
{
	const char *dir = getenv("LTS_PROBE_RESULT_DIR");
	const char *p = getenv("LTS_PROBE_PHASE");
	const char *s = getenv("LTS_PROBE_SONGS");
	if (!dir || !p || !s)
		return false;
	result_dir = QString::fromLocal8Bit(dir);
	phase = QString::fromLocal8Bit(p);
	songs = QString::fromLocal8Bit(s);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	return true;
}
