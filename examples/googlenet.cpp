// Copyright Lin Min 2015

#include <mpi.h>
#include "common/common.hpp"
#include "dispatch/runnable.hpp"
#include "composite/composite.hpp"

namespace purine {

int batch_size = 128;
string source = "/temp/imagenet-train-256xN-lmdb";
string mean_file = "/temp/imagenet-train-mean";

class GoogLeNet : public Graph {
 protected:
  Blob* data_;
  Blob* label_;
  vector<Blob*> weights_;
  vector<Blob*> weight_data_;
  vector<Blob*> weight_diff_;
  vector<Blob*> loss_;
 public:
  explicit GoogLeNet(int rank, int device);
  virtual ~GoogLeNet() override {}
  inline const vector<Blob*>& weight_data() { return weight_data_; }
  inline const vector<Blob*>& weight_diff() { return weight_diff_; }
  inline vector<Blob*> data() { return { data_ }; }
  inline vector<Blob*> label() { return { label_ }; }
  inline vector<Blob*> loss() { return loss_; }
};

GoogLeNet::GoogLeNet(int rank, int device) : Graph(rank, device) {
  data_ = create("data", { batch_size, 3, 224, 224 });
  Blob* data_diff_ = create("data_diff", { batch_size, 3, 224, 224 });
  label_ = create("label", { batch_size, 1, 1, 1 });
  // creating layers
  ConvLayer* conv1 = createGraph<ConvLayer>("conv1",
      ConvLayer::param_tuple(3, 3, 2, 2, 7, 7, 64, "relu"));
  PoolLayer* pool1 = createGraph<PoolLayer>("max_pool1",
      PoolLayer::param_tuple("max", 3, 3, 2, 2, 0, 0));
  LRNLayer* norm1 = createGraph<LRNLayer>("norm1",
      LRNLayer::param_tuple(0.0001, 0.75, 5));
  ConvLayer* conv2_reduce = createGraph<ConvLayer>("conv2_reduce",
      ConvLayer::param_tuple(0, 0, 1, 1, 1, 1, 64, "relu"));
  ConvLayer* conv2 = createGraph<ConvLayer>("conv2",
      ConvLayer::param_tuple(1, 1, 1, 1, 3, 3, 192, "relu"));
  LRNLayer* norm2 = createGraph<LRNLayer>("norm2",
      LRNLayer::param_tuple(0.0001, 0.75, 5));
  PoolLayer* pool2 = createGraph<PoolLayer>("max_pool2",
      PoolLayer::param_tuple("max", 3, 3, 2, 2, 0, 0));
  InceptionLayer* inception3a = createGraph<InceptionLayer>("inception3a",
      InceptionLayer::param_tuple(64, 128, 32, 96, 16, 32));
  InceptionLayer* inception3b = createGraph<InceptionLayer>("inception3b",
      InceptionLayer::param_tuple(128, 192, 96, 128, 32, 64));
  PoolLayer* pool3 = createGraph<PoolLayer>("max_pool3",
      PoolLayer::param_tuple("max", 3, 3, 2, 2, 0, 0));
  InceptionLayer* inception4a = createGraph<InceptionLayer>("inception4a",
      InceptionLayer::param_tuple(192, 208, 48, 96, 16, 64));
  InceptionLayer* inception4b = createGraph<InceptionLayer>("inception4b",
      InceptionLayer::param_tuple(160, 224, 64, 112, 24, 64));
  InceptionLayer* inception4c = createGraph<InceptionLayer>("inception4c",
      InceptionLayer::param_tuple(128, 256, 64, 128, 24, 64));
  InceptionLayer* inception4d = createGraph<InceptionLayer>("inception4d",
      InceptionLayer::param_tuple(112, 288, 64, 144, 32, 64));
  InceptionLayer* inception4e = createGraph<InceptionLayer>("inception4e",
      InceptionLayer::param_tuple(256, 320, 128, 160, 32, 128));
  InceptionLayer* inception5a = createGraph<InceptionLayer>("inception5a",
      InceptionLayer::param_tuple(256, 320, 128, 160, 32, 128));
  InceptionLayer* inception5b = createGraph<InceptionLayer>("inception5b",
      InceptionLayer::param_tuple(384, 384, 128, 192, 48, 128));
  GlobalAverageLayer* global_ave = createGraph<GlobalAverageLayer>("global_avg",
      GlobalAverageLayer::param_tuple());
  DropoutLayer* dropout = createGraph<DropoutLayer>("dropout",
      DropoutLayer::param_tuple(0.4, true));
  InnerProdLayer* inner = createGraph<InnerProdLayer>("inner",
      InnerProdLayer::param_tuple(1000));
  SoftmaxLossLayer* softmaxloss = createGraph<SoftmaxLossLayer>("softmaxloss",
      SoftmaxLossLayer::param_tuple(1.));
  // connecting layers
  B{ data_,  data_diff_ } >> *conv1 >> *pool1 >> *norm1 >> *conv2_reduce
  >> *conv2 >> *norm2 >> *pool2 >> *inception3a >> *inception3b >> *pool3
  >> *inception4a >> *inception4b >> *inception4c >> *inception4d
  >> *inception4e >> *inception5a >> *inception5b >> *global_ave
  >> *dropout >> *inner;
  // loss layer
  softmaxloss->set_label(label_);
  B{ inner->top()[0], inner->top()[1] } >> *softmaxloss;
  // loss
  loss_ = { softmaxloss->loss()[0] };
  // weight
  vector<Layer*> layers = { conv1, conv2_reduce, conv2, inception3a,
                            inception3b, inception4a, inception4b, inception4c,
                            inception4d, inception4e, inception5a, inception5b,
                            inner };
  for (auto layer : layers) {
    const vector<Blob*>& w = layer->weight_data();
    weight_data_.insert(weight_data_.end(), w.begin(), w.end());
  }
  for (auto layer : layers) {
    const vector<Blob*>& w = layer->weight_diff();
    weight_diff_.insert(weight_diff_.end(), w.begin(), w.end());
  }
}

}

using namespace purine;

int main(int argc, char** argv) {
  // initilize MPI
  int ret;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &ret);
  // parallels
  vector<pair<int, int> > parallels;
  for (int rank : {0}) {
    for (int device : {0, 1, 2}) {
      parallels.push_back({rank, device});
    }
  }
  // parameter server
  pair<int, int> param_server = {0, -1};
  // fetch image
  FetchImage fetch(source, mean_file, true, true, true, batch_size, 224,
      parallels);
  fetch.run();
  // create data parallelism of GoogLeNet;
  DataParallel<GoogLeNet> parallel_googlenet(parallels, param_server);
  // print_graph(parallel_googlenet.print());
  // do the initialization
  vector<int> indice(58);
  iota(indice.begin(), indice.end(), 0);
  vector<int> weight_indice(58);
  vector<int> bias_indice(58);
  transform(indice.begin(), indice.end(), weight_indice.begin(),
      [](int i)->int {
        return i * 2;
      });
  transform(indice.begin(), indice.end(), bias_indice.begin(),
      [](int i)->int {
        return i * 2 + 1;
      });
  parallel_googlenet.init<Constant>(bias_indice, Constant::param_tuple(0.));
  parallel_googlenet.init<Gaussian>(weight_indice,
      Gaussian::param_tuple(0., 0.05));
  parallel_googlenet.init<Gaussian>({0, 4, 114, 110, 106, 98, 94},
      Gaussian::param_tuple(0., 0.01));
  // iteration
  for (int iter = 0; iter < 10000; ++iter) {
    // feed prefetched data to googlenet
    parallel_googlenet.feed(fetch.images(), fetch.labels());
    // start googlenet and next fetch
    parallel_googlenet.run_async();
    fetch.run_async();
    fetch.sync();
    parallel_googlenet.sync();
  }
  // Finalize MPI
  MPI_Finalize();
}