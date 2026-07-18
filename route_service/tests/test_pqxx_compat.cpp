// 本文件验证每个 libpqxx 消费翻译单元都能安全完成静态对象析构。
#include <pqxx/pqxx>

int main() {
  const pqxx::params parameters{1, "迁移名称"};
  return parameters.size() == 2 ? 0 : 1;
}
