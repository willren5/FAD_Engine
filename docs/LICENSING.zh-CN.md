# 许可怎么选

FAD Engine 的主许可证为 **openFAD 0.1**，采用 Community + Commercial 模式。它是 Source-Available，保留收费产品和闭源分发的独立商业授权，不宣称属于 OSI 开源许可证。

| 使用方式 | Community 许可是否覆盖 |
| --- | --- |
| 阅读、学习、研究、私下修改和评估 | 覆盖，遵守许可中的使用边界。 |
| 免费发布插件，公开该版本完整可构建源码 | 可覆盖，需要包含对应集成代码、构建说明和许可通知。 |
| 免费发闭源二进制 | 不覆盖，需要独立商业授权。 |
| 收费插件、付费服务或商业硬件，即使源码公开 | 不覆盖，需要独立商业授权。 |
| 纯自愿捐赠，不交换功能、下载、更新、提前访问或专属版本 | 可覆盖，不能把付费权益改名为捐赠。 |
| 使用合法取得的插件制作、演出、发行或销售音乐 | 不额外收 Engine 许可费；第三方素材权利仍单独适用。 |

完整对应源码需要能重建正在分发的具体版本。只公开 Engine 本身、把集成代码留成闭源，不满足 Community 分发条件。源码最迟与产品一起提供，并在最后一次分发后至少保留三年，具体以英文许可为准。

FAD Synth 产品、UI、ALT、模型、训练资料、工厂预设、商业素材和账户/商业后端不属于这个仓库的授权范围。第三方依赖的许可独立适用，FAD 授权不能代替 SDK 或其他依赖授权。

英文 [LICENSE](../LICENSE) 是许可正文，本页是说明，不扩大授权。商业用途看 [商业授权说明](../legal/COMMERCIAL-LICENSE.md)。

[OSI 定义](https://opensource.org/osd)要求允许商业使用等权利，因而我们不把上述收费限制标成标准开源；[GitHub 的许可说明](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository)也区分公开仓库与开源许可。

## Engine改进和贡献的商业采用

固定受益主体为 **阿勒泰自由意文化服务有限责任公司**，英文发布名 **FAD Records**，见 [主体说明](../legal/PROJECT-LICENSOR.md)。维护者或贡献者不会因身份自动变成受益主体。

- 主动提交的贡献：按 [CLA1.1](../legal/CONTRIBUTOR-LICENSE-AGREEMENT.md) 明确接受，记录权利人、权限、对应commit/patch、协议与主体hash和第三方披露。公司取得范围内复制、修改、商业使用和再许可权，可集成到收费、闭源FAD Synth；贡献者保留著作权。PR或合并本身不代替授权。
- 按openFAD 0.1公开分发的Engine修改：依LICENSE第7A节作限定改进回授，并随源码发布 [回授声明](../legal/IMPROVEMENT-GRANT-NOTICE.md)。公司使用前仍需核对真实声明、授权能力和范围。
- 未分发的私人改动、独立产品UI/品牌/音乐/素材、不属于Engine修改的集成代码和无关模块不在回授中。完整源码公开义务本身不扩大回授范围。
- 商业客户购买的是使用权，不因此把自己的独立产品回授给FAD；另签协议有明确约定的除外。旧许可证下已经发布且未作回授的工作不能追溯取得；需要另行许可。

保留依法应留的署名和第三方通知；员工、外包、共同作者和第三方权利不能靠本协议凭空取得。公司商业采用以实际有效授权为依据，不能把空模板、未勾选项或机器人状态视为同意。

起草时核对了 [国家版权局著作权法](https://www.ncac.gov.cn/xxfb/flfg/flfg_532/202103/t20210309_50530.html)、[GitHub贡献条款D.6](https://docs.github.com/en/site-policy/github-terms/github-terms-of-service#6-contributions-under-repository-license) 和 [Apache贡献协议机制](https://www.apache.org/licenses/contributor-agreements.html)。这些机制参考不替代具体权利链和法律审核，也没有将本项目改成Apache许可。

### 公司能采用什么

| 情况 | 商业采用依据 |
| --- | --- |
| 公司原有、确实持权的Engine | 自有权利；仍核对第三方边界。 |
| 主动贡献且明确接受CLA1.1 | 对应贡献范围内可商用、闭源集成和再许可，保留必要通知。 |
| 按openFAD 0.1分发的Engine修改，有有效7A回授声明 | 可采用声明覆盖的Engine修改；不扩及整个产品。 |
| 旧fork、没有7A/CLA、仅把代码放在GitHub | 另核对许可证或取得授权，不能追溯推定。 |
| 商业客户自己的UI、品牌、音乐或无关模块 | 购买商业授权不产生反向授权；另有明确协议才可用。 |
| 投稿中含无权许可的第三方内容 | CLA/回授不补足缺失权利，先解决第三方许可。 |
