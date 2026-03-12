Scriptname FertilityCompat_SexLab extends Quest
{ SexLab P+ 兼容层

  配置通过 FertilityNative.GetConfigBool/Float 从 C++ 端读取
  C++ 端在启动时从 Data/SKSE/Plugins/Fertility.json 加载
  不使用 GlobalVariable }


; ═══════════════════════════════════════════════
; 初始化
; ═══════════════════════════════════════════════

function Setup()
    RegisterHooks()
endFunction

function RegisterHooks()
    if !FertilityNative.GetConfigBool("enableSexLab")
        return
    endIf
    if !SexLabUtil.SexLabIsReady()
        return
    endIf

    RegisterForModEvent("HookOrgasmStart", "OnOrgasmStart")
    RegisterForModEvent("SexLabSeparateOrgasm", "OnSLSOOrgasm")
    Debug.Trace("[Fertility] SexLab P+ hooks registered")
endFunction

function Teardown()
    UnregisterForModEvent("HookOrgasmStart")
    UnregisterForModEvent("SexLabSeparateOrgasm")
endFunction


; ═══════════════════════════════════════════════
; P+ Orgasm 回调
; ═══════════════════════════════════════════════

Event OnOrgasmStart(int threadID, bool hasPlayer)
    ; 每次事件读一次配置 (C++ 端已缓存, 几乎零开销)
    if !FertilityNative.GetConfigBool("enableSexLab")
        return
    endIf

    SexLabFramework SexLab = SexLabUtil.GetAPI()
    if !SexLab
        return
    endIf

    SexLabThread thread = SexLab.GetThread(threadID)
    if !thread || thread.GetStatus() < thread.STATUS_INSCENE
        return
    endIf

    Actor[] positions = thread.GetPositions()
    if !positions || positions.Length < 2
        return
    endIf

    int[] sexes = thread.GetPositionSexes()
    int[] pairs = CollectInteractionPairs(thread, positions, sexes)

    if pairs.Length < 3
        return
    endIf

    int result = FertilityNative.ProcessSceneBatch(positions, sexes, pairs)
    int insem = Math.LogicalAnd(result, 0xFFFF)
    int conc  = Math.RightShift(result, 16)

    if FertilityNative.GetConfigBool("verbose") && (insem > 0 || conc > 0)
        Debug.Notification("[Fertility] T" + threadID + ": " + insem + " insem, " + conc + " conc")
    endIf
EndEvent


; ═══════════════════════════════════════════════
; SLSO 回调
; ═══════════════════════════════════════════════

Event OnSLSOOrgasm(Form actorRef, int threadID)
    if !FertilityNative.GetConfigBool("enableSexLab")
        return
    endIf

    Actor orgasmer = actorRef as Actor
    if !orgasmer
        return
    endIf

    SexLabFramework SexLab = SexLabUtil.GetAPI()
    if !SexLab
        return
    endIf

    SexLabThread thread = SexLab.GetThread(threadID)
    if !thread || thread.GetStatus() < thread.STATUS_INSCENE
        return
    endIf

    Actor[] positions = thread.GetPositions()
    int[] sexes = thread.GetPositionSexes()

    int orgIdx = thread.GetPositionIdx(orgasmer)
    if orgIdx < 0
        return
    endIf

    int orgSex = sexes[orgIdx]
    bool orgCanDonate = CanDonate(orgSex)
    bool orgIsFemale  = IsFemale(orgSex)

    if !orgCanDonate && !orgIsFemale
        return
    endIf

    int i = 0
    while (i < positions.Length)
        if positions[i] && positions[i] != orgasmer
            int partnerSex = sexes[i]
            int ctype = GetDominantInteraction(thread, positions[i], orgasmer)

            if ctype < 0
                ctype = GetDominantInteraction(thread, orgasmer, positions[i])
            endIf

            if ctype >= 0
                if orgIsFemale && IsFemale(partnerSex)
                    FertilityNative.ProcessPair(positions[i], orgasmer, partnerSex, orgSex, ctype)
                elseIf orgCanDonate && CanReceive(partnerSex)
                    FertilityNative.ProcessPair(positions[i], orgasmer, partnerSex, orgSex, ctype)
                elseIf CanDonate(partnerSex) && CanReceive(orgSex)
                    FertilityNative.ProcessPair(orgasmer, positions[i], orgSex, partnerSex, ctype)
                endIf
            endIf
        endIf
        i += 1
    endWhile
EndEvent


; ═══════════════════════════════════════════════
; 交互对收集
; ═══════════════════════════════════════════════

int[] function CollectInteractionPairs(SexLabThread thread, Actor[] positions, int[] sexes)
    int[] pairs = Utility.CreateIntArray(60)
    int idx = 0

    int i = 0
    while (i < positions.Length)
        if positions[i] && CanReceive(sexes[i])
            idx = TryAddPair(thread, positions, sexes, i, 1, pairs, idx)
            idx = TryAddPair(thread, positions, sexes, i, 2, pairs, idx)
            idx = TryAddPair(thread, positions, sexes, i, 3, pairs, idx)
            idx = TryAddPair(thread, positions, sexes, i, 4, pairs, idx)
        endIf
        i += 1
    endWhile

    if idx == 0
        return Utility.CreateIntArray(0)
    endIf

    int[] result = Utility.CreateIntArray(idx)
    int j = 0
    while (j < idx)
        result[j] = pairs[j]
        j += 1
    endWhile
    return result
endFunction

int function TryAddPair(SexLabThread thread, Actor[] positions, int[] sexes, int recvIdx, int ctype, int[] pairs, int idx)
    if idx >= 57
        return idx
    endIf

    Actor partner = none

    if thread.IsInteractionRegistered()
        partner = thread.GetPartnerByType(positions[recvIdx], ctype)
    else
        partner = FallbackFindPartner(thread, positions, sexes, recvIdx, ctype)
    endIf

    if !partner
        return idx
    endIf

    int donorIdx = FindActorIndex(positions, partner)
    if donorIdx < 0 || donorIdx == recvIdx
        return idx
    endIf

    int donorSex = sexes[donorIdx]
    bool isFF = IsFemale(sexes[recvIdx]) && IsFemale(donorSex)
    if !isFF && !CanDonate(donorSex)
        return idx
    endIf

    pairs[idx]     = recvIdx
    pairs[idx + 1] = donorIdx
    pairs[idx + 2] = ctype
    return idx + 3
endFunction

Actor function FallbackFindPartner(SexLabThread thread, Actor[] positions, int[] sexes, int recvIdx, int ctype)
    bool typeMatch = false
    if ctype == 1
        typeMatch = thread.IsSceneVaginal()
    elseIf ctype == 2
        typeMatch = thread.IsSceneAnal()
    elseIf ctype == 3
        typeMatch = thread.IsSceneOral()
    elseIf ctype == 4
        return none
    endIf

    if !typeMatch
        return none
    endIf

    int i = 0
    while (i < positions.Length)
        if positions[i] && i != recvIdx
            if IsFemale(sexes[recvIdx]) && IsFemale(sexes[i])
                return positions[i]
            endIf
            if CanDonate(sexes[i])
                return positions[i]
            endIf
        endIf
        i += 1
    endWhile
    return none
endFunction

int function FindActorIndex(Actor[] positions, Actor target)
    int i = 0
    while (i < positions.Length)
        if positions[i] == target
            return i
        endIf
        i += 1
    endWhile
    return -1
endFunction


; ═══════════════════════════════════════════════
; 主交互类型判定
; ═══════════════════════════════════════════════

int function GetDominantInteraction(SexLabThread thread, Actor receiver, Actor partner)
    if thread.IsInteractionRegistered()
        if thread.HasInteractionType(1, receiver, partner)
            return 1
        endIf
        if thread.HasInteractionType(2, receiver, partner)
            return 2
        endIf
        if thread.HasInteractionType(4, receiver, partner)
            return 4
        endIf
        if thread.HasInteractionType(3, receiver, partner)
            return 3
        endIf
    else
        if thread.IsSceneVaginal()
            return 1
        endIf
        if thread.IsSceneAnal()
            return 2
        endIf
        if thread.IsSceneOral()
            return 3
        endIf
    endIf
    return -1
endFunction


; ═══════════════════════════════════════════════
; 性别判断
; ═══════════════════════════════════════════════

bool function CanReceive(int sex)
    return sex == 1 || sex == 2 || sex == 4
endFunction

bool function CanDonate(int sex)
    return sex == 0 || sex == 2 || sex == 3
endFunction

bool function IsFemale(int sex)
    return sex == 1 || sex == 4
endFunction


; ═══════════════════════════════════════════════
; 性别查询 (P+ aware)
; ═══════════════════════════════════════════════

int function GetActorGender(Actor akActor)
    if FertilityNative.GetConfigBool("enableSexLab")
        if SexLabUtil.SexLabIsReady()
            SexLabFramework SexLab = SexLabUtil.GetAPI()
            if SexLab
                return SexLab.GetSex(akActor)
            endIf
        endIf
    endIf
    return akActor.GetLeveledActorBase().GetSex()
endFunction
