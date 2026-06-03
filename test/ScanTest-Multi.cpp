#include <iostream>
#include <filesystem>
#include <cstdint>
#include <chrono>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    fs::path target_dir = (argc > 1) ? argv[1] : "."; 

    std::cout << "スキャン開始: " << target_dir << "\n";
    auto start_time = std::chrono::high_resolution_clock::now();

    // 💡 複数のスレッドから同時に書き換えても安全なカウンター（Atomic）
    std::atomic<uintmax_t> total_size{0};
    std::atomic<uint64_t> file_count{0};

    // 探索予定のフォルダリスト
    std::vector<fs::path> dirs_to_process;
    dirs_to_process.push_back(target_dir);

    // 💡 スレッド間の交通整理をするためのロック（Mutex）と信号機（Condition Variable）
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<int> active_tasks{0};
    bool done = false;

    // 作業員（スレッド）が実行する関数
    auto worker = [&]() {
        while (true) {
            fs::path current_dir;
            
            // 1. リストからフォルダを1つ安全に取り出す（ロック状態）
            {
                std::unique_lock<std::mutex> lock(mtx);
                // リストに仕事が来るか、全作業が終了するまで待機
                cv.wait(lock, [&]() { return !dirs_to_process.empty() || done; });

                if (done && dirs_to_process.empty()) {
                    return; // 仕事が完全に終わったらスレッド終了
                }

                current_dir = dirs_to_process.back();
                dirs_to_process.pop_back();
                active_tasks++; // 作業中フラグを立てる
            }

            // 2. フォルダの中身を調べる（ここは他のスレッドと並行して全力で処理！）
            std::error_code ec;
            uintmax_t local_size = 0;
            uint64_t local_count = 0;
            std::vector<fs::path> new_dirs;

            for (auto it = fs::directory_iterator(current_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
                if (ec) continue;

                if (it->is_directory(ec)) {
                    if (!it->is_symlink(ec)) {
                        new_dirs.push_back(it->path());
                    }
                } else if (it->is_regular_file(ec)) {
                    local_size += it->file_size(ec);
                    local_count++;
                }
            }

            // 見つけたサイズとファイル数を安全に合算
            total_size += local_size;
            file_count += local_count;

            // 3. 見つけた新しいフォルダをリストに追加し、他の待機中のスレッドを起こす
            {
                std::lock_guard<std::mutex> lock(mtx);
                for (auto& d : new_dirs) {
                    dirs_to_process.push_back(std::move(d));
                }
                active_tasks--;
                
                // もしリストが空っぽで、誰も作業していなければ終了の合図を出す
                if (dirs_to_process.empty() && active_tasks == 0) {
                    done = true;
                }
            }
            cv.notify_all(); // 休んでいるスレッド全員に「仕事追加されたよ！」と通知
        }
    };

    // 💡 あなたのPCのCPUコア数を取得して、その数だけスレッド（作業員）を生成
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; // 取得失敗時の保険
    
    std::cout << "使用スレッド数: " << num_threads << "\n";

    std::vector<std::thread> threads;
    for (unsigned int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker);
    }

    // 全てのスレッドの仕事が終わるのを待つ
    for (auto& t : threads) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "スキャン完了!\n";
    std::cout << "ファイル数: " << file_count << " 個\n";
    
    double size_mb = static_cast<double>(total_size.load()) / (1024.0 * 1024.0);
    double size_gb = size_mb / 1024.0;
    std::cout << "合計サイズ: " << size_mb << " MB (" << size_gb << " GB)\n";
    std::cout << "かかった時間: " << duration.count() << " ミリ秒\n";

    return 0;
}