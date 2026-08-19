#include <preview/preview.hpp>

int main() {
  auto engine = preview::Engine::create();
  return engine ? 0 : 1;
}
