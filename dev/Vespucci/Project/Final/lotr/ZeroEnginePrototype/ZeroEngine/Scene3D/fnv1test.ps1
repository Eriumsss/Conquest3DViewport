function FNV1Hash([string]$str) {
    [uint32]$hash = 0x811c9dc5
    foreach ($ch in $str.ToLower().ToCharArray()) {
        [uint32]$b = [byte][char]$ch
        $hash = [uint32](($hash * 0x01000193) -band 0xFFFFFFFF)
        $hash = $hash -bxor $b
    }
    return $hash
}

$banks = @('Init','BaseCombat','Effects','UI','Music','Ambience','SFXWarg','SFXHorse','SFXTroll',
    'HeroAragorn','HeroGandalf','HeroGimli','HeroLegolas','HeroFrodo','HeroSauron','HeroSaruman',
    'HeroElrond','HeroIsildur','HeroLurtz','HeroMouth','HeroNazgul','HeroWitchKing','HeroWormtongue',
    'Level_Shire','Level_Moria','Level_Trng','Level_HelmsDeep','Level_Isengard','Level_MinasTir',
    'Level_MinasMorg','Level_Osgiliath','Level_Pelennor','Level_Rivendell','Level_BlackGates',
    'Level_Weathertop',
    'SFXBallista','SFXBalrog','SFXBatteringRam','SFXCatapult','SFXEagle','SFXEnt','SFXFellBeast',
    'SFXOliphant','SFXSiegeTower','Creatures',
    'ChatterElf','ChatterOrc','ChatterHeroAragorn','VoiceOver',
    'VO_Shire','VO_Moria','VO_Trng')

foreach ($b in $banks) {
    $h = FNV1Hash $b
    Write-Host ("{0} -> {1}" -f $b, $h)
}

