#include <cassert>
#include <chrono>
#include <thread>
#include "ai/ai_ui_operation.h"
#include "ai/ai_availability.h"
#include "ui_dispatcher.h"
using namespace ai;
static InvokeRequest Req(uint32_t ms=1000){return {"test","{}","",ms,Availability::Get().Generation()};}
int main(){
 auto& a=Availability::Get(); a.ReleaseOwner("test"); int calls=0,cancels=0;
 auto block=a.AcquireBlock("test"); auto r=UiOperations::Submit(Req(),[&]{++calls;OperationResult x;x.status=OperationStatus::Succeeded;return x;});ui_test::RunPosts();ui_test::Tick();assert(calls==0&&UiOperations::GetResult(r.operation_id).status==OperationStatus::Failed);a.ReleaseBlock(block);
 auto stale=Req(); auto change=a.AcquireBlock("test");a.ReleaseBlock(change);auto s=UiOperations::Submit(stale,[&]{++calls;return OperationResult{};});ui_test::RunPosts();ui_test::Tick();assert(calls==0&&UiOperations::GetResult(s.operation_id).status==OperationStatus::Failed);
 auto p=UiOperations::Submit(Req(),[&]{++calls;OperationResult x;x.status=calls>1?OperationStatus::Succeeded:OperationStatus::Pending;return x;});ui_test::RunPosts();ui_test::Tick();ui_test::Tick();assert(UiOperations::GetResult(p.operation_id).status==OperationStatus::Succeeded);
 auto before=UiOperations::Submit(Req(),[&]{++calls;return OperationResult{};},[&]{++cancels;});assert(UiOperations::Cancel(before.operation_id));ui_test::RunPosts();ui_test::Tick();assert(cancels==0&&UiOperations::GetResult(before.operation_id).status==OperationStatus::Cancelled);
 auto after=UiOperations::Submit(Req(),[]{OperationResult x;x.status=OperationStatus::Pending;return x;},[&]{++cancels;});ui_test::RunPosts();ui_test::Tick();assert(UiOperations::Cancel(after.operation_id));ui_test::Tick();assert(cancels==1&&UiOperations::GetResult(after.operation_id).status==OperationStatus::Cancelled);
 ui_test::SetPost(false);auto unavailable=UiOperations::Submit(Req(),[]{return OperationResult{};});assert(unavailable.status==OperationStatus::Failed);ui_test::SetPost(true);ui_test::SetTimer(false);auto no_timer=UiOperations::Submit(Req(),[]{return OperationResult{};});ui_test::RunPosts();assert(UiOperations::GetResult(no_timer.operation_id).status==OperationStatus::Failed);ui_test::SetTimer(true);
 auto timeout=UiOperations::Submit(Req(100),[]{OperationResult x;x.status=OperationStatus::Pending;return x;},[&]{++cancels;});ui_test::RunPosts();ui_test::Tick();std::this_thread::sleep_for(std::chrono::milliseconds(150));ui_test::Tick();assert(UiOperations::GetResult(timeout.operation_id).status==OperationStatus::Failed&&cancels==2);
 auto x=UiOperations::GetResult(p.operation_id);auto y=UiOperations::GetResult(p.operation_id);assert(x.status==y.status);return 0; }
