Scriptname FertilityNative Hidden
{ C++ Native 函数声明
  所有配置通过 JSON (Data/SKSE/Plugins/Fertility.json) 管理
  不使用 GlobalVariable }


; ═══════════════════════════════════════════════
; 场景批处理
;
; 返回: 低 16 位 = 射入数, 高 16 位 = 受孕数
; 解包: Math.LogicalAnd(ret, 0xFFFF) / Math.RightShift(ret, 16)
; ═══════════════════════════════════════════════

int function ProcessSceneBatch(Actor[] actors, int[] sexes, int[] pairs) global native
int function ProcessPair(Actor recipient, Actor donor, int recipientSex, int donorSex, int ctype) global native


; ═══════════════════════════════════════════════
; 查询
; ═══════════════════════════════════════════════

float function GetInflation(Actor akActor) global native
float function GetSpermVolume(Actor akActor) global native
bool function IsPregnant(Actor akActor) global native
float function GetPregnancyProgress(Actor akActor) global native


; ═══════════════════════════════════════════════
; 准入管理
; ═══════════════════════════════════════════════

bool function IsEligible(Actor akActor) global native
function AllowActor(Actor akActor) global native
function ExcludeActor(Actor akActor) global native


; ═══════════════════════════════════════════════
; 配置 (替代 GlobalVariable)
;
; key 名与 JSON 字段一致:
;   bool:  "enableSexLab", "verbose", "eventMessages"
;   float: "cycleDuration", "gestationDays", "recoveryDays",
;          "defaultSpermLifeHours", "spermFullVolume", ...
; ═══════════════════════════════════════════════

bool function GetConfigBool(string key) global native
float function GetConfigFloat(string key) global native
function SetConfigBool(string key, bool val) global native
function SetConfigFloat(string key, float val) global native
function ReloadConfig() global native
function SaveConfig() global native

; Scene
int Function ProcessSceneBatch(Actor[] actors, int[] sexes, int[] pairs) Global Native
int Function ProcessPair(Actor recipient, Actor donor, int recipientSex, int donorSex, int ctype) Global Native

; Query
bool Function IsPregnant(Actor actor) Global Native
float Function GetPregnancyProgress(Actor actor) Global Native
float Function GetInflation(Actor actor) Global Native
float Function GetSpermVolume(Actor actor) Global Native
bool Function IsEligible(Actor actor) Global Native
int Function GetStatus(Actor actor) Global Native

; Action
int Function ForcePregnancy(Actor recipient, Actor father) Global Native
bool Function ForceOvulation(Actor actor) Global Native
bool Function Abort(Actor actor) Global Native
bool Function ForceLabor(Actor actor) Global Native
bool Function SetPhase(Actor actor, int phase) Global Native
bool Function AdvancePregnancy(Actor actor, float days) Global Native
bool Function ClearAll(Actor actor) Global Native
bool Function AbortionDrug(Actor actor) Global Native

; Whitelist
Function AllowActor(Actor actor) Global Native
Function ExcludeActor(Actor actor) Global Native

; Config
Function ReloadConfig() Global Native


int status = FertilityNative.GetStatus(akTarget)
int phase = Math.LogicalAnd(status, 0xF)           ; 0-3
bool pregnant = Math.LogicalAnd(status, 0x10) != 0
bool inLabor = Math.LogicalAnd(status, 0x20) != 0
int progress = Math.RightShift(Math.LogicalAnd(status, 0xFF0000), 16)  ; 0-100
