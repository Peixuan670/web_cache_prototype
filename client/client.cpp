#include <iostream>
#include <fstream>
#include <stdio.h>
#include <queue>
#include <unordered_map>
#include <vector>
#include <utility>
#include <curl/curl.h>
#include <thread>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <atomic>
#include <string>
#include <stdlib.h>
#include <math.h>
using namespace std;

volatile bool queueFull;
volatile bool running;
mutex urlMutex;
mutex histMutex;
mutex printFailConnect;
queue<string> urlQueue;
char *path;
string cacheip;
ofstream outTp;

std::atomic<long> bytes;
std::atomic<long> reqs;

unordered_map<double, long> histData; // the response time histogram of all requests

static size_t throw_away(void *ptr, size_t size, size_t nmemb, void *data)
{
  (void)ptr;
  (void)data;
  return (size_t)(size * nmemb);
}

void histogram(double val)
{
  /*
   * record in hisogram 
   */
  histMutex.lock();
  histData[round(val * 10) / 10.0]++;
  histMutex.unlock();
}

int measureThread()
{
  /**
   * consumer thread of urlQueue
   * send request for url and record response info
   */
  string currentID;

  CURL *curl_handle = nullptr;
  /* init the curl session */
  curl_handle = curl_easy_init();
  /*include header pragmas*/
  struct curl_slist *headers = nullptr; // init to NULL is important
  curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
  /* no progress meter please */
  curl_easy_setopt(curl_handle, CURLOPT_NOPROGRESS, 1L);
  /* send all data to this function  */
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, throw_away);

  while (!queueFull || !::urlQueue.empty())
  {
    if (!running)
      return 0;
    urlMutex.lock();
    if (!::urlQueue.empty())
    {
      currentID = ::urlQueue.front();
      urlQueue.pop();
      urlMutex.unlock();
    }
    else
    {
      urlMutex.unlock();
      this_thread::sleep_for(chrono::milliseconds(10)); //wait for urlQueue reload
      continue;
    }
    /* set URL to get */
    curl_easy_setopt(curl_handle, CURLOPT_URL, (cacheip + currentID).c_str());
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10L);
    //fetch URL
    CURLcode res;
    chrono::high_resolution_clock::time_point start; // the time send request
    chrono::high_resolution_clock::time_point end;   // the time retrived the request

    // if couldn't connect, try 10 times
    for (int failc = 0; failc < 10; failc++)
    {
      //profile latency and perform
      start = chrono::high_resolution_clock::now();
      res = curl_easy_perform(curl_handle);
      end = chrono::high_resolution_clock::now();
      if (res == CURLE_OK) // recieved
        break;
      else if (res == CURLE_COULDNT_CONNECT)
        this_thread::sleep_for(chrono::milliseconds(1)); //wait a little bit
      else
      {
        printFailConnect.lock();
        printFailConnect.unlock();
        continue; //fail and don't try again
      }
    }

    //get elapsed time
    const long timeElapsed_ns = chrono::duration_cast<chrono::nanoseconds>(end - start).count();
    histogram(log10(double(timeElapsed_ns))); // record to histogram

    double content_length = 0.0;
    res = curl_easy_getinfo(curl_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_length);
    if ((CURLE_OK == res) && (content_length > 0.0))
    {
      bytes += (long)content_length;
      reqs++;
      printFailConnect.lock();
      printFailConnect.unlock();
    }
    currentID.clear();
  }

  /* cleanup curl stuff */
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl_handle);

  return 0;
}

int requestCreate()
{
  /**
   * producer of urlQueue, reads requests from trace file line by line and fills into urlQueue
   */
  ifstream infile;
  infile.open(path);
  unordered_map<long, long> osizes;
  long time, id;
  while (infile >> time >> id)
  {
    if (urlQueue.size() > 1000)
    {
      this_thread::sleep_for(chrono::milliseconds(10)); // wait for consumer threads consume queue
    }
    urlMutex.lock();
    urlQueue.push(to_string(id)); // request object id as url
    urlMutex.unlock();
  }
  return 0;
}

void output()
{
  /**
   * output `reqs` and `bytes` augment during last 1 second
   */
  while (running)
  {
    chrono::high_resolution_clock::time_point start = chrono::high_resolution_clock::now();
    reqs.store(0);
    bytes.store(0); // reset `reqs` and `bytes` to 0
    this_thread::sleep_for(chrono::milliseconds(1000));
    const long tmpr = reqs.load();
    const long tmpb = bytes.load();
    chrono::high_resolution_clock::time_point end = chrono::high_resolution_clock::now();
    const long timeElapsed = chrono::duration_cast<chrono::milliseconds>(end - start).count();
    outTp << tmpr << " " << tmpb << " " << timeElapsed << endl;
    std::cerr << tmpr << endl;
  }
}

int main(int argc, char *argv[])
{
  // parse parameters
  if (argc != 6)
  {
    cerr << "three params: path noThreads cacheIP outTp outHist" << endl;
    return 1;
  }
  path = argv[1];
  const int numberOfThreads = atoi(argv[2]);
  cacheip = argv[3];
  if (cacheip.back() != '/')
  { // the request url should be like 127.0.0.1:7000/id, do not miss the '/'
    cacheip.push_back('/');
  }

  bytes.store(0);
  reqs.store(0);

  outTp.open(argv[4]);

  // init curl
  curl_global_init(CURL_GLOBAL_ALL);

  //perform measurements in different threads, save time stamps (global - rough, local - exact)
  cerr << "Starting threads" << endl;
  queueFull = false;
  ::running = true;
  thread threads[numberOfThreads];
  thread outputth = thread(output);
  //starting consumer(send request) threads
  for (int i = 0; i < numberOfThreads; i++)
  {
    threads[i] = thread(measureThread);
  }
  // start creating queue
  chrono::high_resolution_clock::time_point ostart = chrono::high_resolution_clock::now();

  requestCreate(); // start producer in main thread
  queueFull = true;
  cerr << "Finished queue\n";
  for (int i = 0; i < numberOfThreads; i++)
  {
    threads[i].join(); // kill subthread
  }

  chrono::high_resolution_clock::time_point oend = chrono::high_resolution_clock::now();
  long otimeElapsed = chrono::duration_cast<chrono::milliseconds>(oend - ostart).count();
  ::running = false;
  cerr << "Duration: " << otimeElapsed << endl;
  outputth.join();
  cerr << "Finished threads\n";
  curl_global_cleanup();

  ofstream outHist;
  outHist.open(argv[5]); // store histogram data to log file
  for (auto it : histData)
    outHist << it.first << " " << it.second << endl;

  return 0;
}
