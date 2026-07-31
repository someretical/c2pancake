void cleanup(void) {}

// #include <stdint.h>
// uint64_t __c2pnk_external_entry_point_init(void) {
//   cleanup();
//   return 0;
// }

// [[clang::annotate("__c2pnk_`external_entry_point")]]
// void init(void) {
//   (void)__c2pnk_external_entry_point_init();
// }

[[clang::annotate("__c2pnk_external_entry_point")]]
void init(void) {
  cleanup();
}