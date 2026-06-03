#include <iostream>
#include <filesystem>
#include <cstdint>
#include <chrono>
#include <vector> // 配列（スタック）を使うために追加

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    fs::path target_dir = (argc > 1) ? argv[1] : "."; 
    
    uintmax_t total_size = 0;
    uint64_t file_count = 0;

    std::cout << "スキャン開始: " << target_dir << "\n";
    auto start_time = std::chrono::high_resolution_clock::now();

    // 探索予定のフォルダを保持するリスト（スタック）
    std::vector<fs::path> dirs_to_process;
    dirs_to_process.push_back(target_dir);

    // 探索予定リストが空になるまでループ
    while (!dirs_to_process.empty()) {
        // リストの最後から1つフォルダを取り出す
        fs::path current_dir = dirs_to_process.back();
        dirs_to_process.pop_back();

        // 💡ポイント：エラーを「例外（強制終了）」ではなく、この変数に受け取る
        std::error_code ec; 

        // current_dir の中身を1階層だけループ（recursiveではない）
        for (auto it = fs::directory_iterator(current_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
            // もしアクセス拒否や特殊ファイルでエラーが起きたら、無視して次へ
            if (ec) {
                continue; 
            }

            // フォルダの場合
            if (it->is_directory(ec)) {
                // シンボリックリンク（ショートカット）を追うと無限ループになるので除外
                if (!it->is_symlink(ec)) {
                    dirs_to_process.push_back(it->path()); // 次探索するリストに追加
                }
            } 
            // 通常のファイルの場合
            else if (it->is_regular_file(ec)) {
                total_size += it->file_size(ec);
                file_count++;
            }
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "スキャン完了!\n";
    std::cout << "ファイル数: " << file_count << " 個\n";
    
    double size_mb = static_cast<double>(total_size) / (1024.0 * 1024.0);
    double size_gb = size_mb / 1024.0; // GBもあった方が分かりやすいので追加
    std::cout << "合計サイズ: " << size_mb << " MB (" << size_gb << " GB)\n";
    std::cout << "かかった時間: " << duration.count() << " ミリ秒\n";

    return 0;
}