#include <stdlib.h>
#include <time.h>
#include <random>
#include <chrono>
#include <sys/time.h>
#include <ctime>
#include <thread>
#include <algorithm>
#include <math.h>
#include <pthread.h>
#include <atomic>

#include <array>
#include "transaction.h"

#define N 5          // databaseのサイズ
#define NUM_THREAD 2 // Thread数

int commitcounter = 0;
int txcounter = 0;

class txtabledef
{
public:
    int t_sstamp;
    int t_cstamp;
};

std::vector<class txtabledef> transaction_table; // timestamp store
pthread_mutex_t txtablemtx_;                     // transaction_table用lock

std::atomic<int> timestamp(1);
std::atomic<int> TID(10000000);

// std::vector<std::vector<class nodedef>> database(N, std::vector<nodedef>(0)); // database
// nodedef database[N]; // database
std::array<class nodedef, N> database;
pthread_mutex_t dbmtx_; // database用のlock

//------------------------------------------------
//  初期化フェーズ
//------------------------------------------------

// databaseを生成して初期化
void generate_database(void)
{
    /*nodedef tmp;
    for (int i = 0; i < N; i++)
    {
        tmp.v_etastamp = 0;
        tmp.v_pistamp = INFINITY;
        // tmp.v_prev = NULL;
        tmp.tuple = i;
        tmp.key = 0;
        tmp.v_cstamp = 0;
        database.at(i).push_back(tmp);
    }*/

    for (int i = 0; i < N; i++)
    {
        database[i].v_etastamp = 0;
        database[i].v_pistamp = std::numeric_limits<float>::infinity();
        database[i].v_prev = NULL;
        database[i].tuple = i;
        database[i].key = 0;
        database[i].v_cstamp = 0;
        database[i].v_prev = NULL;
    }
    for (int i = 0; i < N; i++)
    {
        nodedef *tmp;
        tmp->v_etastamp = 0;
        tmp->v_pistamp = std::numeric_limits<float>::infinity();
        tmp->v_prev = NULL;
        tmp->tuple = i;
        tmp->key = 0;
        tmp->v_cstamp = 0;
        database[i].v_prev = tmp;
    }
    return;
}

// min_valからmax_valの範囲で乱数を生成する関数
uint64_t get_rand(uint64_t min_val, uint64_t max_val)
{
    static std::mt19937_64 mt64(time(NULL));
    std::uniform_int_distribution<uint64_t> get_rand_uni_int(min_val, max_val);
    return get_rand_uni_int(mt64);
}

// transactionの生成
txdef generate_transaction()
{
    txdef tx;
    int operation_size = get_rand(MIN_NUM_OPERATION, MAX_NUM_OPERATION);
    tx.t_sstamp = 0;
    tx.status = txstatus::inflight;
    tx.t_pistamp = INFINITY; // ここがうまくいってない

    tx.t_cstamp = 0;
    tx.t_etastamp = 0;

    for (int i = 0; i < operation_size; i++)
    {
        ope tmp;
        if (get_rand(0, 100) % 2 == 0)
        {
            tmp.operation = txop::READ;
            tmp.key = 0;
        }
        else
        {
            tmp.operation = txop::WRITE;
            tmp.key = get_rand(0, 100);
        }
        tmp.tuple = get_rand(0, N - 1);
        tx.txinfo.push_back(tmp);
    }
    return tx;
}

void start_transaction(txdef *tx) // transactionのstart timestampを取得
{
    tx->t_sstamp = std::atomic_fetch_add(&timestamp, 1);
    tx->tid = std::atomic_fetch_add(&TID, 1);
    tx->t_cstamp = tx->tid;
}

nodedef *specify_latestnodeversion(txdef *tx, int openum) // readを行うnode, overwriteするnodeを取得
{
    /*nodedef *version = &database.at(tx->txinfo.at(openum).tuple).front();
    for (int v = 0; v < database.at(tx->txinfo.at(openum).tuple).size(); v++)
    {
        if (database.at(tx->txinfo.at(openum).tuple).at(v).v_cstamp < tx->t_sstamp && database.at(tx->txinfo.at(openum).tuple).at(v).initialized == 1)
        {
            version = &database.at(tx->txinfo.at(openum).tuple).at(v);
        }
    }
    return version;*/
    nodedef *version = &database[tx->txinfo.at(openum).tuple];
    while (database[tx->txinfo.at(openum).tuple].initialized == 0 || database[tx->txinfo.at(openum).tuple].v_cstamp >= tx->t_sstamp)
    {
        version = version->v_prev;
    }
    return version;
}

void verify_exclusion_or_abort(txdef *tx) // exclusion window testing
{
    if (tx->status == txstatus::inflight)
    {
        if (tx->t_pistamp <= tx->t_etastamp)
        {
            tx->status = txstatus::aborted;
        }
    }
}

void ssn_write(txdef *tx, nodedef *version, int i) // 論文に準拠
{
    // if v not in t.writes:
    // update \eta(t) with w:r edge
    tx->t_etastamp = std::max<int>(tx->t_etastamp, version->v_etastamp);
    // t.writes.add(v)
    nodedef *tmp;
    tmp->key = tx->txinfo.at(i).key;
    tmp->v_cstamp = tx->tid; // tidに設定
    tmp->tuple = tx->txinfo.at(i).tuple;
    tmp->v_prev = version;
    tmp->v_etastamp = tx->t_etastamp;
    tmp->v_pistamp = tx->t_pistamp;
    tmp->initialized = 0; // not initialized
    database[i].v_prev = tmp;

    tx->wworker.__emplace_back(*tmp);

    // t.reads.discard(v); avoid false positive
    for (auto p = tx->rworker.begin(); p != tx->rworker.end(); p++)
    {
        if (p == tx->wworker.end())
        {
            tx->rworker.erase(p);
        }
    }
    // verify_exclusion_or_abort(t)
    verify_exclusion_or_abort(tx);
}

void ssn_read(txdef *tx, nodedef *version, int num) // 論文に準拠
{
    if (version->v_cstamp == 0) // 存在しないversionのread operation
    {
        return;
    }
    else
    {
        // if v not in t.writes:(read関数で行う)
        // for (auto p = tx->wworker->begin(); p != tx->wworker->end(); p++)
        for (int i = 0; i < tx->wworker.size(); i++)
        {
            if (&tx->wworker.at(i) == version)
            {
                return;
            }
            else
            {
                //  update \eta(t) with w:r edges
                tx->t_etastamp = std::max(tx->t_etastamp, version->v_cstamp);

                if (version->v_pistamp == INFINITY)
                {
                    // t.reads.add(v);
                    /*readsetdef tmp;
                    tmp.key = version->key;
                    tmp.tuple = version->tuple;
                    tmp.v_pistamp = version->v_pistamp;
                    tx->rworker.push_back(tmp);*/
                    // tx->txinfo.at(num).key = version->key;
                    tx->rworker.__emplace_back(*version);
                }
                else
                {
                    // update \pi(t) with r:w edge
                    tx->t_pistamp = std::min<int>(tx->t_pistamp, version->v_pistamp);
                }
                // verify_exclusion_or_abort(t)
                verify_exclusion_or_abort(tx);
            }
        }
    }
}

void ssn_execution(txdef *tx) // transactionの実行
{
    nodedef *version;

    for (int i = 0; i < tx->txinfo.size(); i++)
    {
        version = specify_latestnodeversion(tx, i);
        if (tx->txinfo.at(i).operation == txop::READ)
        {
            ssn_read(tx, version, i);
        }
        else if (tx->txinfo.at(i).operation == txop::WRITE)
        {
            ssn_write(tx, version, i);
        }
    }
}

void commit(txdef *tx) // transactionをcommit
{
    pthread_mutex_lock(&dbmtx_);
    /*for (auto p = tx->wworker->begin(); p != tx->wworker->end(); p++)
    {
        // database.at(p->tuple).push_back(*p);
        nodedef tmp;
        tmp.v_cstamp = p->v_cstamp;
        tmp.v_etastamp = p->v_etastamp;
        tmp.v_pistamp = p->v_pistamp;
        tmp. = p->v_prev;
        tmp.key = p->key;
        tmp.tuple = p->tuple;
        database.at(tmp.tuple).emplace_back(tmp);
    }*/
    // version creation stampをTID→start timestampに変更
    for (auto p = tx->wworker.begin(); p != tx->wworker.end(); p++)
    {
        p->v_cstamp = tx->t_sstamp;
        p->initialized = 1; // initialized
    }
    pthread_mutex_unlock(&dbmtx_);
}

void setTimestamp(txdef *tx) // timestampを設定
{
    txtabledef tmp;
    tmp.t_sstamp = tx->t_sstamp;
    tmp.t_cstamp = tx->t_cstamp;
    transaction_table.push_back(tmp);
}

void si_commit(txdef *tx)
{
    if (tx->status == txstatus::inflight)
    {
        bool isEmpty = tx->wworker.empty(); //   read only transactionの場合、SIクリア
        if (isEmpty == true)
        {
            tx->t_cstamp = 0;
            return;
        }
        else
        {
            tx->t_cstamp = std::atomic_fetch_add(&timestamp, 1); // commit timestampをとる
            pthread_mutex_lock(&txtablemtx_);                    // timestampのgiant lock
            isEmpty = transaction_table.empty();
            if (isEmpty == false)
            {
                for (auto p = transaction_table.begin(); p != transaction_table.end(); p++)
                {
                    // first committers wins
                    if (tx->t_sstamp < p->t_cstamp)
                    {
                        tx->status = txstatus::aborted;
                    }
                }
            }

            if (tx->status != txstatus::aborted)
            {
                setTimestamp(tx); // transaction_tableに追加
            }
            pthread_mutex_unlock(&txtablemtx_);
            return;
        }
    }
    commit(tx);
    tx->status = txstatus::committed;
    // commitcounter++;
}

/*void initialize_version_committed(txdef *tx, int openum)
{
    nodedef *version = &database.at(tx->txinfo.at(openum).tuple).front();
    for (int v = 0; v < database.at(tx->txinfo.at(openum).tuple).size(); v++)
    {
        for (int i = 0; i < tx->wworker->size(); i++)
        {
            if (database.at(tx->txinfo.at(openum).tuple).at(v).key == tx->wworker->at(i).key && database.at(tx->txinfo.at(openum).tuple).at(v).v_cstamp == tx->tid)
            {
                database.at(tx->txinfo.at(openum).tuple).at(v).v_cstamp = std::atomic_fetch_add(&timestamp, 1);
            }
        }
    }
}*/

void ssn_commit(txdef *tx) // 論文に準拠
{

    if (tx->status == txstatus::inflight)
    {
        tx->t_cstamp = std::atomic_fetch_add(&timestamp, 1);
    }
    pthread_mutex_lock(&dbmtx_);

    // finalize \pi(T)
    tx->t_pistamp = std::min<int>(tx->t_pistamp, tx->t_cstamp);
    for (auto p = tx->rworker.begin(); p != tx->rworker.end(); p++)
    {
        tx->t_pistamp = std::min<int>(tx->t_pistamp, p->v_pistamp);
    }

    // finalize \eta(T)
    for (auto p = tx->wworker.begin(); p != tx->wworker.end(); p++)
    {
        tx->t_etastamp = std::max<int>(tx->t_etastamp, p->v_prev->v_etastamp);
    }

    // verify_exclusion_or_abort(t)
    verify_exclusion_or_abort(tx); // exclusion window testing
    if (tx->status == txstatus::inflight)
    {
        tx->status = txstatus::committed;
    }

    // commit
    commit(tx);
    if (tx->status == txstatus::inflight)
    {
        tx->status = txstatus::committed;
    }

    // transactionの処理完了フラグ
    if (tx->status == txstatus::committed)
    {
        commitcounter++;
    }

    if (tx->status == txstatus::committed) // post-commit begins
    {

        // update \eta(v)
        for (auto p = tx->rworker.begin(); p != tx->rworker.end(); p++)
        {
            p->v_etastamp = std::max<int>(p->v_etastamp, tx->t_cstamp);
        }
        // update \pi(v)
        for (auto p = tx->wworker.begin(); p != tx->wworker.end(); p++)
        {
            p->v_prev->v_pistamp = tx->t_pistamp;
            //   initialize new version
            p->v_cstamp = p->v_etastamp = tx->t_cstamp;
        }

        /*for (int i = 0; i < tx->wworker->size(); i++)
        {
            initialize_version_committed(tx, i);
        }*/
    }
    pthread_mutex_unlock(&dbmtx_);
}

//------------------------------------------------
//  結果表示(printf database, transaction...)
//------------------------------------------------

void show_database(void)
{
    std::cout << " " << std::endl;
    std::cout << "-------database--------" << std::endl;
    for (int i = 0; i < N; i++)
    {
        printf("vector[");
        printf("%d", i);
        printf("] ");
        nodedef *tmp;
        tmp = database[i].v_prev;
        while (tmp->v_prev != NULL)
        {
            printf("%d (s(%d),π(%f),η(%d))     ", tmp->key, tmp->v_cstamp, tmp->v_pistamp, tmp->v_etastamp);
            tmp = tmp->v_prev;
        }
        std::cout << " " << std::endl;
        std::cout << " " << std::endl;
    }
}

void show_txtable(void)
{
    std::cout << "----保存されているtimestamp----" << std::endl;
    for (auto p = transaction_table.begin(); p != transaction_table.end(); p++)
    {
        printf("(%d,%d)", p->t_sstamp, p->t_cstamp);
    }
}
void show_transaction(std::vector<std::vector<class txdef>> txtx)
{
    for (auto p = txtx.begin(); p != txtx.end(); p++)
    {
        for (auto q = p->begin(); q != p->end(); q++)
        {
            std::cout << "--------NEW TRANSACTION--------" << std::endl;
            if (q->status == txstatus::committed)
            {
                std::cout << "transaction status: committed" << std::endl;
            }
            else if (q->status == txstatus::aborted)
            {
                std::cout << "transaction status: aborted" << std::endl;
            }
            else if (q->status == txstatus::inflight)
            {
                std::cout << "transaction status:inflight" << std::endl;
            }

            std::cout << "startTimestamp:" << q->t_sstamp << " commitTimestamp:" << q->t_cstamp << std::endl;
            std::cout << "pistamp:" << q->t_pistamp << " etastamp:" << q->t_etastamp << std::endl;

            for (auto r = q->txinfo.begin(); r != q->txinfo.end(); r++)
            {
                if (r->operation == txop::READ)
                {
                    std::cout << "READ vector[" << r->tuple << "]" << std::endl;
                }
                else
                {
                    std::cout << "WRITE vector[" << r->tuple << "] " << r->key << std::endl;
                }
            }
        }
    }
}

//------------------------------------------------
//  各スレッドの仕事
//------------------------------------------------

void *function(void *arg)
{
    std::vector<txdef> *tx = (std::vector<txdef> *)arg;

    std::this_thread::sleep_for(std::chrono::seconds(1)); // 1秒sleep

    //  transactionの実行
    for (auto p = tx->begin(); p != tx->end(); p++)
    {
        start_transaction(&(*p));
        ssn_execution(&(*p));
        //    si_commit(&(*p));
        //     ssn_commit(&(*p));
    }
    tx->clear(); // transactionの表示
    return NULL;
}

//------------------------------------------------
//  main関数
//------------------------------------------------

int main()
{
    // pthread_mutex_init(&countmtx_, NULL);
    pthread_mutex_init(&dbmtx_, NULL);
    pthread_mutex_init(&txtablemtx_, NULL);

    generate_database();

    pthread_t thread[NUM_THREAD];

    // transactionの生成(txtx=THREADに与えるtransaction data)
    std::vector<std::vector<class txdef>> txtx(NUM_THREAD, std::vector<txdef>(0));
    for (int i = 0; i < NUM_THREAD; i++)
    {
        int NUM_transaction = get_rand(MIN_NUM_TRANSACTION, MAX_NUM_TRANSACTION);
        txcounter += NUM_transaction;
        for (int j = 0; j < NUM_transaction; j++)
        {
            txdef tmp;
            tmp = generate_transaction();
            txtx.at(i).push_back(tmp);
        }
    }

    auto start = std::chrono::system_clock::now(); // 計測スタート

    for (int i = 0; i < NUM_THREAD; i++)
    {
        pthread_create(&thread[i], NULL, function, &txtx[i]); // threadの生成
    }

    for (int i = 0; i < NUM_THREAD; i++)
    {
        pthread_join(thread[i], NULL); // threadのjoin
    }

    auto end = std::chrono::system_clock::now(); // 計測終了時刻を保存
    auto dur = end - start;                      // 要した時間を計算
    auto msec = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    // 要した時間をミリ秒（1/1000秒）に変換して表示
    std::cout << msec << " milli sec \n";

    show_transaction(txtx); // transactionの表示
    show_txtable();         // transaction table(timestamp)を表示
    show_database();        // databaseを表示

    // 結果出力
    // std::cout << "commit transaction数:" << commitcounter << std::endl;
    // std::cout << "合計transaction数:" << txcounter << std::endl;
    int abortcounter = txcounter - commitcounter;
    std::cout << "abort率:" << round(double(abortcounter) * 100 / double(txcounter)) << "%" << std::endl;
    int TPS;
    TPS = round(double(commitcounter) / (msec - 1000) * 1000);
    std::cout << "TPS: " << TPS << "tps" << std::endl;

    pthread_mutex_destroy(&dbmtx_);
    pthread_mutex_destroy(&txtablemtx_);

    transaction_table.clear();
    // database.clear();
}